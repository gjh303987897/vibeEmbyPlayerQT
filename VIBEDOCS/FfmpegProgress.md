# FFmpeg 进度行解析

`src/services/ffmpeg/FfmpegProgress.h` 提供无状态的
`FfmpegProgress::parseLine(QByteArray)`，由视频打包和还原共同调用。
它去除首尾空白，按第一个 `=` 分割键和值，返回 `std::optional<Field>`。
空行、缺少分隔符及空键返回空值，不调用负长度的 `QByteArray::first()`。

调用方仍负责 stdout 的行缓冲、所需键选择和数值转换。还原器仅在整数转换
成功且非负时更新帧数和时间；陌生字段忽略。进度输出不改变最终成功判断，
最终结果仍依赖 FFmpeg 退出状态和输出文件。

`AuditRegressionTest::malformedFfmpegProgressIsIgnored` 验证异常行及正常
`out_time_us` 字段；既有打包、还原测试验证完整处理流程。

协议依据：[FFmpeg 的 `-progress` 文档](https://ffmpeg.org/ffmpeg.html)。
