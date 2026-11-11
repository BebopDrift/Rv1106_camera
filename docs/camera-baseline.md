# SC3336 摄像头性能基线

平台为 Luckfox Pico Max（RV1106）和 SC3336，应用为旧版 luckfox_camera + YOLOv5。
SDK 与板端系统信息见 baseline.txt。

| 指标 | 实测结果 | 测量方法 |
|---|---:|---|
| RTSP 平均帧率 | 4.41 FPS | FFmpeg 经 RTSP/TCP 解码 66 帧，视频时间轴跨度 14.967 秒 |
| 进程常驻内存 | 8872 kB | 板端 /proc/PID/status 中的 VmRSS |
| 进程峰值常驻内存 | 10076 kB | 板端 /proc/PID/status 中的 VmHWM |
| rknn_run 平均耗时 | 88.338 ms | 用 steady_clock 在调用前后计时，跳过前 10 次后统计 100 次 |
| rknn_run 最大耗时 | 90.017 ms | 同一批 100 次样本 |

帧率测试连接了一个 RTSP 客户端。推理计时在另一次运行中采集，当时未连接播放器。
