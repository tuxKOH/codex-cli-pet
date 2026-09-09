# Codex Terminal Pet

一个 Linux/X11 原生 C 桌面宠物：检测到 Codex 进程时，只在当前操作的 Codex 终端客户区内显示。渲染使用 Cairo/XRender，窗口和事件使用 Xlib，不依赖 PyQt、PySide 或 Python GUI 框架。

## 运行

```bash
make
./codex-pet
```

安装 X11 登录后自启（脚本会用 `pkexec` 安装 systemd system service，并创建设置 GUI 的桌面快捷方式）：

```bash
./install-systemd.sh
```

脚本会自动通过 `pkexec` 获取安装权限；如果当前系统的 `pkexec` 不可用，也可以执行 `sudo ./install-systemd.sh`，脚本会根据 `SUDO_USER` 安装给原登录用户。不要使用 `sudo -e` 或 `sudo -U`。

若只想查看宠物交互，不启动 Codex，可用：

```bash
make demo
```

## 行为

- 使用 `/proc` 扫描进程树和 X11 的活动窗口；多个 Codex 终端只显示活动的那个。
- 使用 184×184 的 `override_redirect` 透明层，不创建可管理的普通窗口；输入 shape 只覆盖宠物区域，因此点击不会改变 active terminal，也不会把宠物误隐藏。
- 主循环按 16ms 更新，Codex 进程扫描单独缓存 250ms，终端移动和宠物拖动不会被低频进程扫描拖慢。
- 拖动只移动 X11 overlay，不重绘图片；后台 buffer 仅在首次显示、窗口暴露或点击缩放时更新。
- 宠物保存相对终端客户区的位置，终端移动时跟随。
- 终端缩放时宠物尺寸不变；边界碰到时向内推，终端客户区小于宠物时隐藏。
- 左键拖动，松开点击会短暂缩小后恢复。
- 左键点击宠物会异步执行当前配置的 curl 并刷新气泡；点击气泡本身只切换显示配置。
- 右键点击宠物或气泡只触发弹跳和音效，不会刷新请求或切换配置。默认音效是仓库内的 `assets/Ya1.mp3`，通过 `ffplay`、`paplay` 或 `mpg123` 异步播放，缺少播放器时不影响主程序。
- 长按宠物约 550ms 会打开显示配置 GUI；也可以从桌面里的 “Codex Pet Settings” 打开。
- Codex 进程结束时 overlay 只隐藏并继续等待，不退出；下一个匹配的 Codex terminal 出现后会重新显示。
- 已生成 `processed_assets/pet_transparent.png` 和 `processed_assets/chat_bubble_transparent.png` 去背版本，原始资源不会被改写。去背使用全图黑色背景分类，封闭区域也会被处理，不再只从四角洪水填充。

## JSON / 气泡显示

GUI 会写入程序目录下的 `codex-pet.json`。旧版本的 `.env` 会在找不到 JSON 时自动导入并生成 JSON：

```json
{
  "active": 0,
  "sound": "assets/Ya1.mp3",
  "displays": [
    {
      "name": "余额",
      "curl": "curl -s 'https://newapi.example.com/api/user/self'",
      "json": "data.balance",
      "template": "余额：$content",
      "value": ""
    }
  ]
}
```

点击气泡会在配置之间循环，点击人物才会请求当前配置。长按人物打开 GUI，可新增、编辑和删除配置。JSON 解析支持 `data.balance`，也支持 `{"data":{"balance":$content}}` 这种模板写法；显示模板里的 `$content` 会替换为解析结果。

显示模板支持基础运算，例如 `已使用: {$content // 1000}k`。支持 `+`、`-`、`*`、`/`、`//`、`%` 和括号；`//` 是向下取整除法。

点击音效在 JSON 的全局 `sound` 字段中设置，默认是仓库内的 `assets/Ya1.mp3`。设置 GUI 也可以直接编辑这个路径，支持绝对路径、相对于程序目录的路径，以及 `~/...` 路径；留空即可关闭音效。

当前实现依赖 X11 的 `_NET_CLIENT_LIST`、`_NET_ACTIVE_WINDOW` 和 `_NET_WM_PID`，以及 Cairo/X11 开发包。Wayland 会话需要后续增加对应的 compositor/桌面环境适配。

## 致谢

感谢 [MeteorNOX/DeepSeek-Balance-Whale-Widget](https://github.com/MeteorNOX/DeepSeek-Balance-Whale-Widget)。本项目的图片生成参考图和点击音效均来源于该项目。
