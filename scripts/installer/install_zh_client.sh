#!/usr/bin/env bash

# 删除默认的网络配置
adb shell rm //etc/wpa_supplicant.conf

# 推送veq配置
adb push config_aivqe.json //oem/usr/share/vqefiles

# 先给rkipc改名，防止它自启动占用资源
adb shell mv //oem/usr/bin/rkipc //oem/usr/bin/rkipc-bak

# 创建 zh 工作目录
ZH_WORK_BASE=//data/zh_work
adb shell rm -rf $ZH_WORK_BASE
adb shell mkdir -p $ZH_WORK_BASE

# CA 证书目录
ZH_CERT_BASE=$ZH_WORK_BASE/certs
adb shell mkdir -p $ZH_CERT_BASE

# 人脸工作目录
ZH_FACE_WORK_BASE=$ZH_WORK_BASE/face
ZH_FACE_WORK_SAVE=$ZH_FACE_WORK_BASE/save
adb shell mkdir -p $ZH_FACE_WORK_BASE
adb shell mkdir -p $ZH_FACE_WORK_SAVE

# 人脸识别引擎
adb push face_engine $ZH_FACE_WORK_BASE
adb shell chmod +x $ZH_FACE_WORK_BASE/face_engine

# onnx runtime
adb push libonnxruntime.so.1.17.3 $ZH_WORK_BASE

# libbithion-core.so.1
adb push libbithion-core.so.1 $ZH_WORK_BASE

# HTTPS CA bundle
adb push certs/ca-certificates.pem $ZH_CERT_BASE/ca-certificates.pem

# zh-key
adb push key $ZH_WORK_BASE

# zh 客户端
adb push zh_client $ZH_WORK_BASE
adb shell chmod +x $ZH_WORK_BASE/zh_client

# 开机启动脚本
adb push S22zh_client //etc/init.d/
adb shell chmod +x //etc/init.d/S22zh_client

adb push start_zh_client.sh $ZH_WORK_BASE
adb shell chmod +x $ZH_WORK_BASE/start_zh_client.sh

# 杀守护进程/客户端脚本
adb push kill.sh $ZH_WORK_BASE
adb shell chmod +x $ZH_WORK_BASE/kill.sh

# 蓝牙配网相关
BLE_SH=start_ble_provision.sh
adb push $BLE_SH $ZH_WORK_BASE
adb shell chmod +x $ZH_WORK_BASE/$BLE_SH

BLE_GATT_SERVER=zh_ble_gatt_server
adb push $BLE_GATT_SERVER $ZH_WORK_BASE
adb shell chmod +x $ZH_WORK_BASE/$BLE_GATT_SERVER

# 系统提示音
adb push prompt_mp3 $ZH_WORK_BASE

adb shell reboot
