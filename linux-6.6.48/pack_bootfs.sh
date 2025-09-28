#!/bin/bash
#本脚本用于打包linux镜像，生成bootfs ext4磁盘文件，提供参考
#执行本脚本需要root权限

#检查输入内存容量参数: 1:DDR_1GB; 2:DDR_2GB
if [ $# -eq 0 ]; then
    read -p "请选择DDR内存容量, 输入数字1或2, 按Enter键确认, 开始打包封装:
1.DDR_1GB
2.DDR_2GB
输入数字: " ddr_size_number
    if [ $ddr_size_number -ne 1 ] && [ $ddr_size_number -ne 2 ]; then
        echo "error:unsupport"
        exit 1
    fi
else 
    echo "usage example:./pack_bootfs.sh"
    exit 1
fi

#检查是否存在存放linux编译结果的build_image目录，不存在则退出
build_image_dir=$PWD/../build_image
if [ ! -e $build_image_dir ]; then
    echo "find $build_image_dir folder failed"
    exit 1
fi

#检查是否已存在bootfs ext4文件，若存在则先删除
if [ -e $build_image_dir/bootfs-1GB.ext4 ];then
    rm -rf $build_image_dir/bootfs-1GB.ext4
fi

if [ -e $build_image_dir/bootfs-2GB.ext4 ];then
    rm -rf $build_image_dir/bootfs-2GB.ext4
fi

#检查build_image目录下是否存在内核镜像Image.gz、设备树文件、内核驱动模块目录
#首先检查是否存在内核镜像文件，不存在则退出打包
if [ ! -e $build_image_dir/Image.gz ];then
    echo "find $build_image_dir/Image.gz file failed"
    exit 1
else
    #检查设备树二进制文件数量，若数量为0,则退出打包
    dtb_count=$(ls $build_image_dir/*.dtb 2>/dev/null | wc -l)
    if [ $dtb_count -eq 0 ];then
        echo "find $build_image_dir/*.dtb file failed"
        exit 1
    else
        #检查是否存在内核模块文件，不存在则退出打包
        if [ ! -e $build_image_dir/lib/modules/6.6.48 ];then
            echo "find $build_image_dir/lib/modules/6.6.48 folder failed"
            exit 1
        fi
    fi
fi

#创建一个bootfs.ext4磁盘文件，空间大小为192MB
sudo dd if=/dev/zero of=$build_image_dir/bootfs.ext4 bs=1M count=192
sudo mkfs.ext4 -L bootfs $build_image_dir/bootfs.ext4

#创建bootfs.ext4的临时挂载目录bootfs_mount
bootfs_mount_tmp=bootfs_mount
sudo mkdir $build_image_dir/$bootfs_mount_tmp

#将bootfs.ext4挂载到bootfs_mount目录，接着拷贝所有文件到该目录下
sudo mount $build_image_dir/bootfs.ext4 $build_image_dir/$bootfs_mount_tmp

#将内核镜像、设备树文件、内核驱动模块拷贝到bootfs_mount目录
sudo cp $build_image_dir/Image.gz $build_image_dir/$bootfs_mount_tmp
sudo cp $build_image_dir/*.dtb $build_image_dir/$bootfs_mount_tmp
sudo cp -a $build_image_dir/lib/modules/6.6.48 $build_image_dir/$bootfs_mount_tmp

#将其他所需文件拷贝到bootfs_mount目录
sudo cp bootfs_config/boot.scr.uimg $build_image_dir/$bootfs_mount_tmp
sudo cp bootfs_config/st-image-resize-initrd $build_image_dir/$bootfs_mount_tmp

#拷贝extlinux文件,并调整为指定文件名
#1)先清理
if [ -e "$build_image_dir/$bootfs_mount_tmp/mmc0_extlinux/" ]; then
    sudo rm -rf $build_image_dir/$bootfs_mount_tmp/mmc0_extlinux/
fi

if [ ! -e "$build_image_dir/$bootfs_mount_tmp/mmc1_extlinux/" ]; then
    sudo rm -rf $build_image_dir/$bootfs_mount_tmp/mmc1_extlinux/
fi
#2)创建目录
sudo mkdir $build_image_dir/$bootfs_mount_tmp/mmc0_extlinux/
sudo mkdir $build_image_dir/$bootfs_mount_tmp/mmc1_extlinux/
#3)拷贝
if [ $ddr_size_number -eq 1 ]; then
    #DDR 1GB
    sudo cp bootfs_config/mmc0_extlinux/extlinux-1GB.conf $build_image_dir/$bootfs_mount_tmp/mmc0_extlinux/extlinux.conf
    sudo cp bootfs_config/mmc1_extlinux/extlinux-1GB.conf $build_image_dir/$bootfs_mount_tmp/mmc1_extlinux/extlinux.conf
elif [ $ddr_size_number -eq 2 ]; then
    #DDR 2GB
    sudo cp bootfs_config/mmc0_extlinux/extlinux-2GB.conf $build_image_dir/$bootfs_mount_tmp/mmc0_extlinux/extlinux.conf
    sudo cp bootfs_config/mmc1_extlinux/extlinux-2GB.conf $build_image_dir/$bootfs_mount_tmp/mmc1_extlinux/extlinux.conf
else
    #非正确选项
    echo "error:unsupport"
    exit 1
fi

#取消挂载，并删除临时挂载目录，完成bootfs.ext4磁盘内容更新
sudo umount $build_image_dir/$bootfs_mount_tmp
sudo rm -rf $build_image_dir/$bootfs_mount_tmp

#最后根据选择的DDR容量配置，调整bootfs.ext4名字
if [ $ddr_size_number -eq 1 ]; then
    #DDR 1GB
    sudo mv $build_image_dir/bootfs.ext4 $build_image_dir/bootfs-1GB.ext4
elif [ $ddr_size_number -eq 2 ]; then
    #DDR 2GB
    sudo mv $build_image_dir/bootfs.ext4 $build_image_dir/bootfs-2GB.ext4
else
    #非正确选项
    echo "error:unsupport"
    exit 1
fi

#提示打包结束
echo "---bootfs ext4 package finish---"
