# libvterm 0.3.3

The build sources and [original MIT license](LICENSE) come from the upstream libvterm 0.3.3 release archive.<br>
构建源码和[原始 MIT 许可证](LICENSE)来自 libvterm 0.3.3 上游发布归档。

Upstream archive: [libvterm-0.3.3.tar.gz](https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz)<br>
上游归档：[libvterm-0.3.3.tar.gz](https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz)

SHA-256: `09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0`

Local changes in `src/parser.c`, `src/vterm_internal.h`, and `src/state.c` bound escape parameters and keep repeated glyphs within the available screen cells.<br>
本地针对 `src/parser.c`、`src/vterm_internal.h` 和 `src/state.c` 的修改限制了转义序列参数，并防止重复字符超出屏幕可用单元格。
