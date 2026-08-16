# Third-Party Notices

本文件列出本仓库分发产物中随附的第三方依赖的许可证信息(依赖 / 版本 / 许可证 / URL 四列,12 §1.1)。
完整条目与逐条许可证兼容性审计在仓库转 public 之前完成(12 §1.3 / §8 待验证表)。

| 依赖                          | 版本          | 许可证      | URL                                             |
| ----------------------------- | ------------- | ----------- | ----------------------------------------------- |
| Space Grotesk(`web/fonts/`) | Version 2.000 | OFL-1.1     | https://github.com/floriankarsten/space-grotesk |
| IBM Plex Sans(`web/fonts/`) | Version 3.201 | OFL-1.1     | https://github.com/IBM/plex                     |
| IBM Plex Mono(`web/fonts/`) | Version 2.3   | OFL-1.1     | https://github.com/IBM/plex                     |
| Noto Sans SC(`web/fonts/`)  | Version 2.004 | OFL-1.1     | https://github.com/notofonts/noto-cjk           |
| <依赖名>                      | <版本>        | <SPDX 标识> | <上游 URL>                                      |

字体四条(T27)是 `text=` 子集产物,由 `scripts/fetch_fonts.py` 生成,随插件二进制分发 ——
OFL-1.1 要求随字体分发版权声明,故它们不能等到转 public 再补。版本号取自各 `.woff2` 的
name ID 5;`REUSE.toml` 的 `web/fonts/**` 特例块与 `web/fonts/README.md` 都指向本表。
