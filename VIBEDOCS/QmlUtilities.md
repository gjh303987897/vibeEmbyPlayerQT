# QML 辅助组件

这些组件在 Main.qml 中创建，但当前业务页面没有调用其处理接口；修复
潜伏缺陷不代表页面已经接入它们。复杂业务仍应使用 C++ Service/ViewModel。

`BindingOptimizer.debounce` 创建单次 Timer，直接连接函数对象，保留捕获的
变量。它自动启动，返回 Timer 供调用者取消；触发后在 finally 中销毁。
取消且不再使用的 Timer 可由调用者销毁，不把函数序列化为 QML 字符串。

`DataProcessManager` 给请求分配 ID；WorkerScript 原样返回 ID，只发布同类
操作的最新结果。不同操作分别跟踪；失败通过 requestFailed 报告。过滤器
只接受预定义名称；custom 不再执行字符串，未知动作/过滤器返回错误。

`ImageCacheManager` 保留自己创建的 Image，而不是预加载器即将销毁的 Image。
清理时销毁这些缓存对象。尺寸未知、超出容量的图片不进入缓存；按估计
RGBA 大小和条目数双重限制，默认 100 MiB/256 项。此估计不代表 Qt 全局
pixmap 缓存或进程内存的严格上限。getCachedImage 仍返回原有缓存记录。

`QmlUtilityTest` 在 Qt Quick 软件后端运行，验证闭包/Timer 回收、旧请求丢弃、
custom 拒绝、原 Image 销毁后缓存仍有效以及清缓存时对象销毁。
