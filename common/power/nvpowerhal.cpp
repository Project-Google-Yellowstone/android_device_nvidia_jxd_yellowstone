/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (c) 2012-2014, NVIDIA CORPORATION.  All rights reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "powerHAL::common"

#include <hardware/hardware.h>
#include <hardware/power.h>

#include "powerhal_utils.h"
#include "powerhal.h"

#include "nvos.h"

#define PHS_DEBUG

static int get_input_count(void)
{
    int i = 0;
    int ret;
    char path[80];
    char name[32];

    while(1)
    {
        sprintf(path, "/sys/class/input/input%d/name", i);
        ret = access(path, F_OK);
        if (ret < 0)
            break;
        memset(name, 0, 32);
        sysfs_read(path, name, 32);
        ALOGI("input device id:%d present with name:%s", i++, name);
    }
    return i;
}

static void find_input_device_ids(struct powerhal_info *pInfo)
{
    int i = 0;
    int status;
    int count = 0;
    char path[80];
    char name[MAX_CHARS];

    while (1)
    {
        sprintf(path, "/sys/class/input/input%d/name", i);
        if (access(path, F_OK) < 0)
            break;
        else {
            memset(name, 0, MAX_CHARS);
            sysfs_read(path, name, MAX_CHARS);
            for (int j = 0; j < pInfo->input_cnt; j++) {
                status = (-1 == pInfo->input_devs[j].dev_id)
                    && (0 == strncmp(name,
                    pInfo->input_devs[j].dev_name, MAX_CHARS));
                if (status) {
                    ++count;
                    pInfo->input_devs[j].dev_id = i;
                    ALOGI("find_input_device_ids: %d %s",
                        pInfo->input_devs[j].dev_id,
                        pInfo->input_devs[j].dev_name);
                }
            }
            ++i;
        }

        if (count == pInfo->input_cnt)
            break;
    }
}

static int check_hint(struct powerhal_info *pInfo, power_hint_t hint, uint64_t *t)
{
    struct timespec ts;
    uint64_t time;

    if (hint >= POWER_HINT_COUNT)
        return -1;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    time = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;

    if (pInfo->hint_time[hint] && pInfo->hint_interval[hint] &&
        (time - pInfo->hint_time[hint] < pInfo->hint_interval[hint]))
        return -1;

    *t = time;

    return 0;
}

void common_power_camera_init(struct powerhal_info *pInfo, camera_cap_t *cap)
{
    char const* dlsym_error;
    int target;
    int i;

    if (!pInfo)
    {
        ALOGE("pInfo is NULL");
        return;
    }

    memset(&pInfo->camera_power, 0, sizeof(pInfo->camera_power));

    pInfo->camera_power.fd_gpu = -1;
    pInfo->camera_power.fd_cpu_freq_max = -1;
    pInfo->camera_power.fd_min_online_cpus = -1;

    pInfo->camera_power.target_fps = 0;
    pInfo->camera_power.cam_cap = cap;
    pInfo->camera_power.usecase_index = -1;

    /* initalization primitives for regular hints thread */
    pthread_cond_init(&pInfo->wait_cond,NULL);
    pthread_mutex_init(&pInfo->wait_mutex,NULL);
    pInfo->regular_hints_thread = 0;
    pInfo->exit_hints_thread = false;

    return;

err_camera_init:
    if (pInfo->camera_power.handle)
        dlclose(pInfo->camera_power.handle);
}

static bool is_available_frequency(struct powerhal_info *pInfo, int freq)
{
    int i;

    for(i = 0; i < pInfo->num_available_frequencies; i++) {
        if(pInfo->available_frequencies[i] == freq)
            return true;
    }

    return false;
}

void common_power_open(struct powerhal_info *pInfo)
{
    int i;
    int size = 256;
    char *pch;

    if (0 == pInfo->input_devs || 0 == pInfo->input_cnt)
        pInfo->input_cnt = get_input_count();
    else
        find_input_device_ids(pInfo);

    // Initialize timeout poker
    Barrier readyToRun;
    pInfo->mTimeoutPoker = new TimeoutPoker(&readyToRun);
    readyToRun.wait();

    // Read available frequencies
    char *buf = (char*)malloc(sizeof(char) * size);
    sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies",
               buf, size);

    // Determine number of available frequencies
    pch = strtok(buf, " ");
    pInfo->num_available_frequencies = -1;
    while(pch != NULL)
    {
        pch = strtok(NULL, " ");
        pInfo->num_available_frequencies++;
    }

    // Store available frequencies in a lookup array
    pInfo->available_frequencies = (int*)malloc(sizeof(int) * pInfo->num_available_frequencies);
    sysfs_read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies",
               buf, size);
    pch = strtok(buf, " ");
    for(i = 0; i < pInfo->num_available_frequencies; i++)
    {
        pInfo->available_frequencies[i] = atoi(pch);
        pch = strtok(NULL, " ");
    }

    // Store LP cluster max frequency
    sysfs_read("/sys/devices/system/cpu/cpuquiet/tegra_cpuquiet/idle_top_freq",
                buf, size);
    pInfo->lp_max_frequency = atoi(buf);

    pInfo->interaction_boost_frequency = pInfo->lp_max_frequency;
    pInfo->animation_boost_frequency = pInfo->lp_max_frequency;

    for (i = 0; i < pInfo->num_available_frequencies; i++)
    {
        if (pInfo->available_frequencies[i] >= 1326000) {
            pInfo->interaction_boost_frequency = pInfo->available_frequencies[i];
            break;
        }
    }

    for (i = 0; i < pInfo->num_available_frequencies; i++)
    {
        if (pInfo->available_frequencies[i] >= 1326000) {
            pInfo->animation_boost_frequency = pInfo->available_frequencies[i];
            break;
        }
    }

    // Initialize hint intervals in usec
    //
    // Set the interaction timeout to be slightly shorter than the duration of
    // the interaction boost so that we can maintain is constantly during
    // interaction.
    pInfo->hint_interval[POWER_HINT_INTERACTION] = 90000;
    pInfo->hint_interval[POWER_HINT_APP_PROFILE] = 200000;
    pInfo->hint_interval[POWER_HINT_APP_LAUNCH] = 1500000;
    pInfo->hint_interval[POWER_HINT_SHIELD_STREAMING] = 500000;
    pInfo->hint_interval[POWER_HINT_HIGH_RES_VIDEO] = 500000;
    pInfo->hint_interval[POWER_HINT_MIRACAST] = 500000;
    pInfo->hint_interval[POWER_HINT_DISPLAY_ROTATION] = 500000;

    // Initialize AppProfile defaults
    pInfo->defaults.min_freq = 0;
    pInfo->defaults.fan_cap = 70;
    pInfo->defaults.power_cap = 0;
    pInfo->defaults.gpu_cap = UINT_MAX;

    // Initialize fds
    pInfo->fds.app_min_freq = -1;
    pInfo->fds.app_max_gpu = -1;
    pInfo->fds.app_min_gpu = -1;

    // Initialize features
    pInfo->features.fan = sysfs_exists("/sys/devices/platform/pwm-fan/pwm_cap");

    free(buf);
}

static void set_cpu_scaling_min_freq(struct powerhal_info *pInfo, int value)
{
    if (value < 0)
        value = pInfo->defaults.min_freq;

    if (pInfo->fds.app_min_freq >= 0) {
        close(pInfo->fds.app_min_freq);
        pInfo->fds.app_min_freq = -1;
    }
    pInfo->fds.app_min_freq = pInfo->mTimeoutPoker->requestPmQos("/dev/cpu_freq_min", value);

    ALOGV("%s: set min CPU floor =%d", __func__, value);
}

static void set_gpu_scaling(struct powerhal_info *pInfo, int value)
{
    if (pInfo->fds.app_min_gpu >= 0) {
        close(pInfo->fds.app_min_gpu);
        pInfo->fds.app_min_gpu = -1;
    }

    if (value)
        pInfo->fds.app_min_gpu = pInfo->mTimeoutPoker->requestPmQos("/dev/gpu_freq_min", 0);
    else
        pInfo->fds.app_min_gpu = pInfo->mTimeoutPoker->requestPmQos("/dev/gpu_freq_min", INT_MAX);
}

static void set_gpu_core_cap(struct powerhal_info *pInfo, int value)
{
    if (value < 0)
        value = pInfo->defaults.gpu_cap;

#ifdef GPU_IS_GK20A
    if (pInfo->fds.app_max_gpu >= 0) {
        close(pInfo->fds.app_max_gpu);
        pInfo->fds.app_max_gpu = -1;
    }

    pInfo->fds.app_max_gpu = pInfo->mTimeoutPoker->requestPmQos("/dev/gpu_freq_max", value);
#else
    sysfs_write_int("sys/kernel/tegra_cap/cbus_cap_state", 1);
    sysfs_write_int("sys/kernel/tegra_cap/cbus_cap_level", value);
#endif
}

static void set_pbc_power(struct powerhal_info *pInfo, int value)
{
    if (value < 0)
        value = pInfo->defaults.power_cap;

    set_property_int(POWER_CAP_PROP, value);
}

static void set_fan_cap(struct powerhal_info *pInfo, int value)
{
    if (!pInfo->features.fan)
        return;

    if (value < 0)
        value = pInfo->defaults.fan_cap;

    sysfs_write_int("/sys/devices/platform/pwm-fan/pwm_cap", value);
}

static void set_camera_cpu_freq_max(struct powerhal_info *pInfo, int value)
{
    ALOGV("%s: %d", __func__, value);
    if (value > 0)
    {
        if (pInfo->camera_power.fd_cpu_freq_max != -1)
        {
            close(pInfo->camera_power.fd_cpu_freq_max);
        }
        pInfo->camera_power.fd_cpu_freq_max =
            pInfo->mTimeoutPoker->requestPmQos("/dev/cpu_freq_max", value);
    }
}

static void set_camera_min_online_cpus(struct powerhal_info *pInfo, int value)
{
    ALOGV("%s: %d", __func__, value);
    if (pInfo->camera_power.fd_min_online_cpus != -1)
    {
        close(pInfo->camera_power.fd_min_online_cpus);
    }
    pInfo->camera_power.fd_min_online_cpus =
        pInfo->mTimeoutPoker->requestPmQos("/dev/min_online_cpus", value);
}

static void set_camera_max_online_cpus(struct powerhal_info *pInfo, int value)
{
    ALOGV("%s: %d", __func__, value);
    if (pInfo->camera_power.fd_max_online_cpus != -1)
    {
        close(pInfo->camera_power.fd_max_online_cpus);
    }
    pInfo->camera_power.fd_max_online_cpus =
        pInfo->mTimeoutPoker->requestPmQos("/dev/max_online_cpus", value);
}

static void set_camera_fps(struct powerhal_info *pInfo)
{
    int sts;

    if (pInfo->camera_power.fd_gpu == -1)
    {
        pInfo->camera_power.fd_gpu = NvOsSetFpsTarget(pInfo->camera_power.target_fps);

        if (pInfo->camera_power.fd_gpu == -1)
        {
            ALOGE("fail to set camera perf target");
            return;
        }
    }
    else
    {
        sts = NvOsModifyFpsTarget(pInfo->camera_power.fd_gpu, pInfo->camera_power.target_fps);

        if (sts == -1)
        {
            ALOGE("fail to modify camera perf target");
            return;
        }
    }
    ALOGV("%s: set %d fps to GPU FPS target fd=%d", __func__,
        pInfo->camera_power.target_fps, pInfo->camera_power.fd_gpu);
}

static void set_camera_single_phs_hint(NvOsTPHintType hintType, NvU32 value)
{
    int sts = 0;

    sts =  NvOsSendThroughputHint("camera", hintType, value, SLEEP_INTERVAL_SECS*1000);

    if (sts)
    {
        ALOGE("%s: fail to set hint %d, err=%d",  __func__, hintType, sts);
    }
    else
    {
        ALOGV("%s: set hint %d to value=%d", __func__, hintType, value);
    }
}

static void set_camera_phs_hints(struct powerhal_info *pInfo, camera_cap_t *cap)
{
    if (cap->minCpuHint)
    {
        set_camera_single_phs_hint(NvOsTPHintType_MinCPU, cap->minCpuHint);
    }

    if (cap->maxCpuHint)
    {
        set_camera_single_phs_hint(NvOsTPHintType_MaxCPU, cap->maxCpuHint);
    }

    if (cap->minGpuHint)
    {
        set_camera_single_phs_hint(NvOsTPHintType_MinGPU, cap->minGpuHint);
    }

    if (cap->maxGpuHint)
    {
        set_camera_single_phs_hint(NvOsTPHintType_MaxGPU, cap->maxGpuHint);
    }

    if (cap->fpsHint)
    {
#ifdef PHS_DEBUG
        pInfo->camera_power.target_fps = cap->fpsHint;
#else
        set_camera_single_phs_hint(NvOsTPHintType_FramerateTarget, cap->fpsHint);
#endif
    }
}

static void reset_camera_hint(struct powerhal_info *pInfo)
{
    if (pInfo->camera_power.fd_gpu != -1)
    {
        ALOGV("%s: cancel camera perf target", __func__);
        NvOsCancelFpsTarget(pInfo->camera_power.fd_gpu);
        pInfo->camera_power.fd_gpu = -1;
    }
    pInfo->camera_power.target_fps = 0;

    if (pInfo->camera_power.fd_cpu_freq_max != -1)
    {
        close(pInfo->camera_power.fd_cpu_freq_max);
        pInfo->camera_power.fd_cpu_freq_max = -1;
    }

    if (pInfo->fds.app_min_freq >= 0) {
        close(pInfo->fds.app_min_freq);
        pInfo->fds.app_min_freq = -1;
    }

    if (pInfo->camera_power.fd_min_online_cpus != -1)
    {
        close(pInfo->camera_power.fd_min_online_cpus);
        pInfo->camera_power.fd_min_online_cpus = -1;
    }

    if (pInfo->camera_power.fd_max_online_cpus != -1)
    {
        close(pInfo->camera_power.fd_max_online_cpus);
        pInfo->camera_power.fd_max_online_cpus = -1;
    }

    if (pInfo->camera_power.usecase_index != -1)
    {
        pInfo->camera_power.usecase_index = -1;
        NvOsCancelThroughputHint("camera");
    }
}

static void *regular_hints_threadfunc(void *powerInfo)
{
    int err = 0;
    struct powerhal_info *pInfo;
    pInfo = (struct powerhal_info *)powerInfo;
    struct timespec timeout;
    struct timeval tv;

    while (1)
    {
        clock_gettime(CLOCK_MONOTONIC, &timeout);
        timeout.tv_sec += SLEEP_INTERVAL_SECS;
        pthread_mutex_lock(&pInfo->wait_mutex);
        if (pInfo->exit_hints_thread == true)
        {
            pthread_mutex_unlock(&pInfo->wait_mutex);
            break;
        }
        err = pthread_cond_timedwait_monotonic(&pInfo->wait_cond, &pInfo->wait_mutex, &timeout);
        pthread_mutex_unlock(&pInfo->wait_mutex);
        // loop back if timedout
        if (err == ETIMEDOUT)
        {
            if (pInfo->camera_power.target_fps)
            {
                ALOGV("Woken up by Timeout, set fps");
                set_camera_fps(pInfo);
            }

            if ((pInfo->camera_power.usecase_index >= CAMERA_STILL_PREVIEW) &&
                (pInfo->camera_power.usecase_index < CAMERA_USECASE_COUNT))
            {
                ALOGV("Woken up by Timeout, set phs hints");
                set_camera_phs_hints(pInfo, &(pInfo->camera_power.cam_cap[pInfo->camera_power.usecase_index]));
            }

            continue;
        }
        else
        {
            ALOGV("Woken up by signal, exit");
            break;
        }
    }
    ALOGV("Exiting regular hints thread");
    return NULL;

}

static void wait_for_regular_hints_thread(struct powerhal_info *pInfo)
{
    int err = 0;
    ALOGV("Signal regular hints thread");
    if (pInfo->regular_hints_thread)
    {
        pthread_mutex_lock(&pInfo->wait_mutex);
        pInfo->exit_hints_thread = true;
        pthread_mutex_unlock(&pInfo->wait_mutex);
        err = pthread_cond_signal(&pInfo->wait_cond);
    }
    else
    {
        ALOGV("Thread already exited");
        return;
    }
    if (err)
    {
        ALOGE("%s: condition variable not initialized", __func__);
    }
    pthread_join(pInfo->regular_hints_thread, NULL);
    pInfo->regular_hints_thread = 0;
    pInfo->exit_hints_thread = false;
}


static void send_regular_hints(struct powerhal_info *pInfo)
{
    int err = 0;

    // don't create another thread if hints are already being sent
    pthread_mutex_lock(&pInfo->wait_mutex);
    if (pInfo->regular_hints_thread)
    {
        pthread_mutex_unlock(&pInfo->wait_mutex);
        return;
    }
    ALOGV("Creating regular hints thread");
    err = pthread_create(&pInfo->regular_hints_thread, NULL,
            regular_hints_threadfunc, (void *)pInfo);

    pthread_mutex_unlock(&pInfo->wait_mutex);
    if (err)
    {
        ALOGE("%s: failed to create thread, errno = %d", __func__, errno);
    }
}

static void set_camera_hint(struct powerhal_info *pInfo, camera_hint_t *data)
{
    camera_cap_t *cap = NULL;

    ALOGV("%s: setting camera_hint hint = %d", __func__, data[0]);

    switch (data[0]) {
        /* POWER and PERF will be implemented later */
        case CAMERA_HINT_STILL_PREVIEW_POWER:
            pInfo->camera_power.usecase_index = CAMERA_STILL_PREVIEW;
            cap = &(pInfo->camera_power.cam_cap[CAMERA_STILL_PREVIEW]);
            break;
        case CAMERA_HINT_VIDEO_PREVIEW_POWER:
            pInfo->camera_power.usecase_index = CAMERA_VIDEO_PREVIEW;
            cap = &(pInfo->camera_power.cam_cap[CAMERA_VIDEO_PREVIEW]);
            break;
        case CAMERA_HINT_VIDEO_RECORD_POWER:
            pInfo->camera_power.usecase_index = CAMERA_VIDEO_RECORD;
            cap = &(pInfo->camera_power.cam_cap[CAMERA_VIDEO_RECORD]);
            break;
        case CAMERA_HINT_HIGH_FPS_VIDEO_RECORD_POWER:
            pInfo->camera_power.usecase_index = CAMERA_VIDEO_RECORD_HIGH_FPS;
            cap = &(pInfo->camera_power.cam_cap[CAMERA_VIDEO_RECORD_HIGH_FPS]);
            break;
        case CAMERA_HINT_PERF:
            // boost CPU freq to highest for 1s
            pInfo->mTimeoutPoker->requestPmQosTimed("dev/cpu_freq_min",
                                                     pInfo->available_frequencies[pInfo->num_available_frequencies - 1],
                                                     s2ns(1));
            pInfo->mTimeoutPoker->requestPmQosTimed("dev/min_online_cpus",
                                                     2,
                                                     s2ns(1));
            break;
        case CAMERA_HINT_FPS:
            pInfo->camera_power.target_fps = CAMERA_TARGET_FPS;
            set_camera_fps(pInfo);
            ALOGV("%s: set_camera: target_fps = %d", __func__, pInfo->camera_power.target_fps);
            break;
        case CAMERA_HINT_RESET:
            wait_for_regular_hints_thread(pInfo);
            reset_camera_hint(pInfo);
        default:
            break;
    }

    if (cap)
    {
        if (cap->minFreq)
        {
            // floor CPU freq
            set_cpu_scaling_min_freq(pInfo, cap->minFreq);
        }

        if (cap->freq)
        {
            // cap CPU freq
            set_camera_cpu_freq_max(pInfo, cap->freq);
        }

        if (cap->min_online_cpus)
        {
            // cap the number of CPU
            set_camera_min_online_cpus(pInfo, cap->min_online_cpus);
        }

        if (cap->max_online_cpus)
        {
            // cap the number of CPU
            set_camera_max_online_cpus(pInfo, cap->max_online_cpus);
        }

        set_camera_phs_hints(pInfo, cap);
    }

    // launch regular hints thread
    if (pInfo->camera_power.target_fps)
    {
        //lauch thread for fps
        ALOGV("%s: set_camera: lauch regular thread for fps", __func__);
        send_regular_hints(pInfo);
    }
    else
    {
        //lauch thread for phs hints
        if(cap)
        {
            if (cap->minCpuHint || cap->maxCpuHint || cap->minGpuHint ||
                cap->maxGpuHint || cap->fpsHint)
            {
                ALOGV("%s: set_camera: lauch regular thread for phs hints", __func__);
                send_regular_hints(pInfo);
            }
        }
    }
}

static void app_profile_set(struct powerhal_info *pInfo, app_profile_knob *data)
{
    int i;

    for (i = 0; i < APP_PROFILE_COUNT; i++)
    {
        switch (i) {
            case APP_PROFILE_CPU_SCALING_MIN_FREQ:
                set_cpu_scaling_min_freq(pInfo, data[i]);
                break;
            case APP_PROFILE_GPU_CBUS_CAP_LEVEL:
                set_gpu_core_cap(pInfo, data[i]);
                break;
            case APP_PROFILE_GPU_SCALING:
                set_gpu_scaling(pInfo, data[i]);
                break;
            case APP_PROFILE_FAN_CAP:
                set_fan_cap(pInfo, data[i]);
            default:
                break;
        }
    }
}

void common_power_init(struct power_module *module, struct powerhal_info *pInfo)
{
    common_power_open(pInfo);

    pInfo->ftrace_enable = get_property_bool("nvidia.hwc.ftrace_enable", false);

    // Boost to max frequency on initialization to decrease boot time
    pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                     pInfo->available_frequencies[pInfo->num_available_frequencies - 1],
                                     s2ns(15));
}

void common_power_set_interactive(struct power_module *module, struct powerhal_info *pInfo, int on)
{
    int i;
    int dev_id;
    char path[80];
    const char* state = (0 == on)?"0":"1";

    sysfs_write("/sys/devices/platform/host1x/nvavp/boost_sclk", state);

    if (0 != pInfo) {
        for (i = 0; i < pInfo->input_cnt; i++) {
            if (0 == pInfo->input_devs)
                dev_id = i;
            else if (-1 == pInfo->input_devs[i].dev_id)
                continue;
            else
                dev_id = pInfo->input_devs[i].dev_id;
            sprintf(path, "/sys/class/input/input%d/enabled", dev_id);
            if (!access(path, W_OK)) {
                if (0 == on)
                    ALOGI("Disabling input device:%d", dev_id);
                else
                    ALOGI("Enabling input device:%d", dev_id);
                sysfs_write(path, state);
            }
        }
    }

    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq", (on == 0)?"420000":"624000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/target_loads", (on == 0)?"80":"65 228000:75 624000:85");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay", (on == 0)?"80000":"19000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/timer_rate", (on == 0)?"300000":"20000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/boost_factor", (on == 0)?"2":"0");

}

void common_power_hint(struct power_module *module, struct powerhal_info *pInfo,
                            power_hint_t hint, void *data)
{
    uint64_t t;

    if (!pInfo)
        return;

    if (check_hint(pInfo, hint, &t) < 0)
        return;

    switch (hint) {
    case POWER_HINT_VSYNC:
        break;
    case POWER_HINT_INTERACTION:
        if (pInfo->ftrace_enable) {
            sysfs_write("/sys/kernel/debug/tracing/trace_marker", "Start POWER_HINT_INTERACTION\n");
        }
        // Boost to interaction_boost_frequency
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                                 pInfo->interaction_boost_frequency,
                                                 ms2ns(100));
        // During the animation, we need some level of CPU/GPU/EMC frequency floor
        // to get perfect animation. Forcing CPU frequency through PMQoS does not
        // scale EMC fast enough so EMC frequency boosting should be placed first.
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/min_online_cpus",
                                                 2,
                                                 s2ns(2));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                                 pInfo->animation_boost_frequency,
                                                 s2ns(2));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/gpu_freq_min",
                                                 180000,
                                                 s2ns(2));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/emc_freq_min",
                                                 396000,
                                                 s2ns(2));
        break;
    case POWER_HINT_MULTITHREAD_BOOST:
        // Boost to 4 cores
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/min_online_cpus",
                                                 4,
                                                 s2ns(2));
        break;
    case POWER_HINT_APP_LAUNCH:
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                                 INT_MAX,
                                                 ms2ns(1500));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/min_online_cpus",
                                                 2,
                                                 ms2ns(1500));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/gpu_freq_min",
                                                 180000,
                                                 ms2ns(1500));
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/emc_freq_min",
                                                792000,
                                                ms2ns(1500));
        break;
    case POWER_HINT_APP_PROFILE:
        if (data) {
            app_profile_set(pInfo, (app_profile_knob*)data);
        }
        break;
    case POWER_HINT_SHIELD_STREAMING:
        // Boost to 816 Mhz frequency for one second
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                                 816000,
                                                 s2ns(1));
        break;
    case POWER_HINT_HIGH_RES_VIDEO:
        // Boost to max LP frequency for one second
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                                 pInfo->lp_max_frequency,
                                                 s2ns(1));
        break;
    case POWER_HINT_MIRACAST:
        // Boost to 816 Mhz frequency for one second
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                                 816000,
                                                 s2ns(1));
    case POWER_HINT_CAMERA:
        set_camera_hint(pInfo, (camera_hint_t*)data);

        break;
    case POWER_HINT_DISPLAY_ROTATION:
        // Boost to 1.2 Ghz frequency for three seconds
        pInfo->mTimeoutPoker->requestPmQosTimed("/dev/cpu_freq_min",
                                                 1200000,
                                                 s2ns(3));
    default:
        break;
    }

    pInfo->hint_time[hint] = t;
}

