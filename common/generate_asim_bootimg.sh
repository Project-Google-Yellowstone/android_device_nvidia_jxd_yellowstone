#!/bin/bash
#
# Usage: generate_asim_bootimg.sh [target_board] [debug|release]
#        generate_asim_bootimg.sh --help
#
# Description: Generate a boot.img suitable for booting on ASIM
#              with root file system stored on EMMC images.
#
# TODO: How to:
#         1) keep mem= in cmdline in sync w/ actual runs
#         2) keep mmc partition # in sync w/ flash.cfg
#-------------------------------------------------------------------------------

if [ "$1" == "--help" ]; then
    this_script=`basename $0`
    echo " "
    echo "Generate an Android boot image capable of booting on ASIM"
    echo "with root filesystem on EMMC images."
    echo " "
    echo "Usage:"
    echo " "
    echo "  $this_script [target_board] [build_flavor]"
    echo "  $this_script --help"
    echo " "
    echo "where:"
    echo " "
    echo "  target_board: curacao_sim | bonaire_sim | ... "
    echo "  build_flavor: debug | release   (default is debug)"
    echo "  --help:       produces this description"
    echo " "
    exit 0
fi

# Sanity checks & default option processing
if [ "$TOP" == "" ]; then
    echo "ERROR: You must set environment variable TOP to the top of your nvrepo tree"
    exit 2
fi

if [ "$TOP" == "." ]; then
    top=`pwd`
else
    top=$TOP
fi

if [ "$1" == "" ]; then
    echo "Assuming target board is $TARGET_PRODUCT"
    target_board=$TARGET_PRODUCT
else
    target_board=$1
fi

if [ "$2" == "" ]; then
    echo "Assuming build flavor is $TARGET_BUILD_TYPE"
    build_flavor=$TARGET_BUILD_TYPE
else
    build_flavor=$2
fi

if [ "$build_flavor" == "debug" ]; then
    product_dir=$top/out/debug/target/product/$target_board
elif [ "$build_flavor" == "release" ]; then
    product_dir=$top/out/target/product/$target_board
else
    echo "ERROR: Invalid build flavor -- must be debug or release"
    exit 2
fi

device_dir=$top/device/nvidia/$target_board
asim_init_rc=$device_dir/init.asim.rc
asim_init_board_rc=$device_dir/init.$target_board.asim.rc
ramdisk_tmpdir=$product_dir/asim_ramdisk_tmpdir

# Detect the kernel architecture (arm or arm64)
build64=`grep 'CONFIG_ARM64=y' $product_dir/obj/KERNEL/.config`
if [ -z $build64 ]; then
    kernel_arch=arm
else
    kernel_arch=arm64
fi

# Exit when no init.asim.rc file to copy
if [ ! -f "$asim_init_rc" ]; then
    exit 0
fi

# Host tools directory
android_host_bin=$top/out/host/linux-x86/bin

if [ -d "$product_dir" ]; then
    echo "Using $ramdisk_tmpdir to build new ramdisk contents"
    rm -rf $ramdisk_tmpdir
    mkdir -p $ramdisk_tmpdir
else
    echo "ERROR: Product output directory $product_dir does not exist"
    exit 2
fi

# Extract the contents of standard ramdisk image generated by JackBuild
if [ -f "$product_dir/ramdisk.img" ]; then
    echo "Unpacking archive $product_dir/ramdisk.img"
    cd $ramdisk_tmpdir
    gzip -dc < $product_dir/ramdisk.img | cpio --extract
else
    echo "ERROR: No ramdisk.img found in $product_dir"
    exit 2
fi

echo "Replacing init.rc files with asim versions"
echo cp $asim_init_rc $ramdisk_tmpdir/init.rc
cp $asim_init_rc $ramdisk_tmpdir/init.rc
echo cp $asim_init_board_rc $ramdisk_tmpdir/init.$target_board.rc
cp $asim_init_board_rc $ramdisk_tmpdir/init.$target_board.rc
echo "Removing standard components that are useless on sim"
rm -f $ramdisk_tmpdir/init.goldfish.rc
rm -f $ramdisk_tmpdir/ueventd.goldfish.rc
rm -f $ramdisk_tmpdir/init.nv_dev_board.usb.rc

# Assume mmcblk0p1 is the root partition
linux_cmdline='mem=1024M@2048M tegraboot=sdmmc root=/dev/mmcblk0p1 gpt rootwait init=/init'

# Package up the new ramdisk image
if [ -d $android_host_bin ]; then
    echo "Packaging new ramdisk into $product_dir/asim_ramdisk.img"
    cd $ramdisk_tmpdir
    find . | cpio -o -H newc --quiet | gzip >$product_dir/asim_ramdisk.img
    echo "Creating $product_dir/emmc_boot.img"
	$android_host_bin/mkbootimg \
	    --kernel  $product_dir/obj/KERNEL/arch/$kernel_arch/boot/zImage \
	    --ramdisk $product_dir/asim_ramdisk.img \
	    --cmdline "$linux_cmdline" \
	    --output $product_dir/emmc_boot.img
else
    echo "ERROR: Host tools directory $android_host_bin does not exist"
    exit 2
fi

echo "Done"
exit 0
