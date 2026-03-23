#ifndef FACE_RECOGNITION_H
#define FACE_RECOGNITION_H

#include "config.h"
#include "ws.h"

// 启动/停止人脸识别线程（线程常驻，按 VAD active 控制上报）。
int zh_face_recognition_start(void);
void zh_face_recognition_stop(void);

// 设置 VAD 区间 active 状态（1=开始上报，0=停止上报）。
void zh_face_recognition_set_active(int active);

// 绑定当前 WS 会话（用于上报）。
void zh_face_recognition_set_ws(zh_ws_session_t *ws);

// 服务器指令驱动的录入流程。
// 收到 face_recog 时调用，采集平均特征并缓存。
int zh_face_enroll_on_recog(void);
// 收到 face_owner 时调用，使用缓存特征落库并上报。
int zh_face_enroll_on_owner(const char *name);

#endif
