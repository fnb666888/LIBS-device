# 浅色测量工作台

本次调整沿用 Qt 6 Widgets、Fusion 控件风格与项目内 QSS，统一为浅灰背景、白色卡片、蓝色主操作和清晰的文字层级。界面固定浅色，不随 Windows 的深色偏好切换。

UI 仍通过命名管道访问业务层。设备预检、测量启停、参数保存、光谱订阅与导出沿用现有流程；设备驱动和出光判定不迁入界面。

## 参数入口

常用参数集中在“仪器参数”页，数值直接显示单位与范围。

| 设备 | 常用参数 |
| --- | --- |
| 激光器 | 放电电流、放电脉宽、放电频率、放电延时、Q 延时、Q 预热 |
| 光谱仪 | 积分时间、积分延时 |

稳定等待超时、出光保持、最小放电次数和采集超时放入各设备卡片的折叠高级设置。折叠仅影响显示，字段仍参与读取、验证和保存。

范围沿用共享的 `src/include/libs/measurement_limits.hpp`。界面读取业务层的已保存参数；缺失字段由业务层按真机默认值补齐。实时轮询不会覆盖尚未保存的输入。

小窗口下表单纵向滚动，保存与重新读取按钮固定在页底。实时光谱页保留大面积绘图区，调整坐标轴、复选框、曲线颜色和空数据提示。

## 构建和检查

```powershell
pwsh -NoProfile -File ui/build.ps1 -QtPrefix D:\Qt6\6.11.2\mingw_64
```

控件图标通过 Qt Resource 嵌入程序；PowerShell 构建使用 `rcc`，CMake 使用 `AUTORCC`。

本次在 Qt 6.11.2 上编译并检查主页面、参数页、1180×720 小窗口及高级设置展开状态。界面检查连接独立的 `--no-devices` 后端，验证中文、默认值回填和布局，不触发真实设备。截图输出在 `build/ui-light-preview/`。

## Qt 6 方案选择

现有 Widgets 项目采用 Fusion + QSS 即可完成此次界面改版，保留已有信号槽与管道客户端。若未来需要更丰富的动画、触控交互或全面重写前端，可采用 Qt Quick Controls，并考虑 FluentWinUI3 浅色风格；这需要重新实现界面层。

- [Qt Quick Controls 样式](https://doc.qt.io/qt-6/qtquickcontrols-styles.html)
- [FluentWinUI3](https://doc.qt.io/qt-6/qtquickcontrols-fluentwinui3.html)
