/**
  * @file    cmd.h
  * @brief   命令解析 —— 把收到的字节变成动作
  *
  * 这个模块吃进裸字节，吐出动作：拉继电器、换链路、换热点。
  * 它不碰 socket，也不碰 GPIO —— 那两件事交给 link.c 和 relay.c。
  *
  * @par 用法
  *
  * 两个回调分别注册给两个传输模块，然后由 link_init() 启动链路：
  *
  *   @code
  *   tcp_client_set_rx_handler(cmd_on_tcp_rx);
  *   udp_client_set_rx_handler(cmd_on_udp_rx);
  *   link_init(LINK_TCP);
  *   @endcode
  *
  * 两个都要注册 —— 切换时另一条会被启用，那时它需要自己的入口。
  *
  * @note    本模块【不需要初始化】：状态都放在 static 变量里，启动时是零值。
  *
  * @warning 回调进来的 data 是【裸字节流】，不是一条完整消息。本模块内部会
  *          自己攒、自己找关键字，调用方不要试图先"整理"一遍。
  *
  * @see     学习笔记/ESP8266-TCP-UDP-WiFi-STA.md §1.2（拆包与粘包）、
  *          §5.5（命令表为什么要"扫最靠前"）、§6（两个 C 坑）、
  *          学习笔记/ESP8266开发流程.md §9.3（命令表）、§9.4 / §9.5（两个坑的现象与排查）
  */

#pragma once

/**
  * @brief    注册给 tcp_client 的接收回调
  *
  * 🟢 L2 —— 工具：薄包装，判完来源就转给同一份解析逻辑。
  *
  * @param[in] data  收到的字节
  * @param[in] len   字节数
  *
  * @note     不是当前活动链路来的数据会被丢掉（见 link_current()）。
  */
void cmd_on_tcp_rx(const char *data, int len);

/**
  * @brief    注册给 udp_client 的接收回调
  *
  * 🟢 L2 —— 工具：薄包装，和 cmd_on_tcp_rx() 只差来源判断。
  *
  * @param[in] data  这一包的字节
  * @param[in] len   字节数
  *
  * @note     和 cmd_on_tcp_rx() 的区别【只在来源】—— 判断逻辑是同一份。
  */
void cmd_on_udp_rx(const char *data, int len);
