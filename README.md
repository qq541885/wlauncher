# wlauncher — Wine 程序启动器（Switch 中文图形界面）

给 Autorun (wine-nx) 做的前端：扫描 `sdmc:/switch/wine2/drive_c/Programs` 下的
exe / bat，列表选择后写 `target.txt` / `args.txt`，再用 `envSetNextLoad`
跳到 `wine-nx-runtime.nro`。

中文由 Switch 系统 SharedFont 提供，不占 nro 体积。

## 操作
- 方向键：选择
- A：启动
- X：刷新列表
- B：退出

## 构建
devkitPro + `switch-sdl2` + `switch-sdl2_ttf`，`make`。CI 已配好。
