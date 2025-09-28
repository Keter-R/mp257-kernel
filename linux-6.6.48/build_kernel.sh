#!/bin/bash

#判断交叉编译工具链是否存在
if [ ! -e "/opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux" ]; then
    echo "未在该路径下找到交叉编译器:/opt/st/stm32mp2/5.0.3-snapshot/"
    echo "请先安装交叉编译器:atk-image-openstlinux-weston-stm32mp2.rootfs-x86_64-toolchain-5.0.3-snapshot.sh"
    exit 1
fi

#配置交叉编译器
source /opt/st/stm32mp2/5.0.3-snapshot/environment-setup-cortexa35-ostl-linux

#检查输入内存容量参数: 1:DDR_1GB; 2:DDR_2GB
if [ $# -eq 0 ]; then
    read -p "请选择DDR内存容量, 输入数字1或2, 按Enter键确认, 开始编译:
1.DDR_1GB
2.DDR_2GB
输入数字: " ddr_size_number
    if [ $ddr_size_number -ne 1 ] && [ $ddr_size_number -ne 2 ]; then
        echo "error:unsupport"
        exit 1
    fi
else 
    echo "usage example:./build_kernel.sh"
    exit 1
fi

#编译前先清理上一次的编译结果,可选
#由于linux内核源码编译时间较长，可选择不删除上一次编译结果
# if [ -e $PWD/../build ];then
#     rm -rf $PWD/../build
# fi

#执行linux内核源码配置文件stm32mp257_atk_defconfig
make stm32mp257_atk_defconfig O="$PWD/../build"

#若需要更改menuconfig配置，请更改后更新stm32mp257_atk_defconfig配置
#示例：
#make menuconfig O="$PWD/../build"
#cp $PWD/../build/.config arch/arm64/configs/stm32mp257_atk_defconfig

#编译linux内核源码
make Image.gz vmlinux dtbs LOADADDR=0xC2000040 O="$PWD/../build" -j4
#若编译内核源码失败，则退出编译
if [ ! $? -eq 0 ]; then
    exit 1
fi

#编译linux内核模块
make modules O="$PWD/../build" -j4
#若编译内核模块失败，则退出编译
if [ ! $? -eq 0 ]; then
    exit 1
fi

#在上层目录新建一个build_image目录，用于存放编译后的目标文件：内核镜像、设备树文件、内核驱动模块
if [ ! -e "$PWD/../build_image" ]; then
    mkdir $PWD/../build_image
else 
    #如果已存在该目录，则清空该目录
    rm -rf $PWD/../build_image/*
fi

#指定linux内核模块安装目录build_image，安装模块，并移除模块内部调试信息
make INSTALL_MOD_PATH="$PWD/../build_image" modules_install O="$PWD/../build" INSTALL_MOD_STRIP=1 -j4
#若安装内核模块失败，则退出安装
if [ ! $? -eq 0 ]; then
    exit 1
fi

#移除模块目录下的无用链接目录
rm $PWD/../build_image/lib/modules/6.6.48/build -rf
rm $PWD/../build_image/lib/modules/6.6.48/source -rf

#复制编译后的内核镜像到build_image目录下
cp $PWD/../build/arch/arm64/boot/Image.gz $PWD/../build_image/

#复制编译后的设备树文件到build_image目录下
#DDR 1GB设备树（包含多个设备树文件）
devicetree_ddr_1GB=stm32mp257d-atk-ddr-1GB*.dtb
#DDR 2GB设备树（包含多个设备树文件）
devicetree_ddr_2GB=stm32mp257d-atk-ddr-2GB*.dtb

if [ $ddr_size_number -eq 1 ]; then
    #DDR 1GB
    cp $PWD/../build/arch/arm64/boot/dts/st/$devicetree_ddr_1GB $PWD/../build_image/
elif [ $ddr_size_number -eq 2 ]; then
    #DDR 2GB
    cp $PWD/../build/arch/arm64/boot/dts/st/$devicetree_ddr_2GB $PWD/../build_image/
else
    #非正确选项
    echo "error:unsupport"
    exit 1
fi

#提示编译结束，请查看具体编译信息
echo "---Compile finish---"
