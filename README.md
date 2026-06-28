# pickball_scoreboard
## 项目简介
基于ESP32开发的匹克球远程计分系统，裁判通过微信小程序远程下发计分指令，设备接收后实时同步比分到陶晶驰串口屏，赛场观众可直观查看实时分数。

## 核心功能
1. 微信小程序远程控制比分增减、重置、局数切换；
2. WIFI+OneNET MQTT加密通信，指令低延迟下发；
3. ESP32串口驱动陶晶驰显示屏实时刷新比分；
4. 云平台设备密钥、证书做隐私占位处理，开源代码无隐私泄露。

## 技术栈
- 主控：ESP32-S3
- 显示：陶晶驰TJC串口触摸屏
- 通信：MQTT TLS加密、OneNET物联网平台
- 控制端：微信小程序
- 开发框架：ESP-IDF

## 使用方法
1. 修改 components/WIFI/my_wifi.h 内wifi名称，wifi密码参数；
2. 修改 components/ONENET/onenet.h 内产品、设备、密钥参数；
3. idf.py build flash monitor 编译烧录程序；
4. 设备联网后连接云平台，打开配套小程序即可远程计分。
