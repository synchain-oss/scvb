# 界面词条三语交叉核对记录

> 记录 `web/shared/i18n.js` 的 en / fr 词条按 J127 口径做过的核对:以 zh 为准,AI 三语交叉核对(用户 2026-09-28 授权代替法语人工抽检)。
> 九条红字(`guide.title` / `guide.rule1..9`)的审校状态另记在 [`docs/hard-rules.i18n.json`](../hard-rules.i18n.json) 的 `frReview`,发版清单第 5 步查的是那一处。
> 此后新增或改动的 en / fr 词条,补核之后在本文件末尾追加一轮,逐条写「键 / 原文 / 结论 / 改后」。

## 第 1 轮 —— J127(#299,2026-09-28)

- 范围:当时字典里全部 564 个键,另加九条红字。
- 结果:改了 3 处(`tour.step15.body` 与 `wave.tipMinSeg` 意思不一致,`curve.sideTooltip` 的法语冒号前缺空格),九条红字未改。
- 这一轮没有逐条记录,只有摘要:见 `CHANGELOG.md` 0.9.0-rc.1 节「三语词条交叉核对」一条与 frReview 的【J127 轮】。本文件从第 2 轮起建立。

## 第 2 轮 —— J127 之后的新增与改动(2026-09-29)

### 范围与数法

- 基线:`acb2212a`(#299 合并)对 `61b7575d`(#338 合并;开这一轮时 `feature/v1` 的头,其后到本轮提交前没有新的合并)。
- 数法:分别 import 两个提交的 `web/shared/i18n.js`,逐键比较导出字典 `T` 的 zh / en / fr 值。结果:en / fr 各新增 13 个键、改动 13 个键(两种语言是同一批键),没有删键;键总数 564 → 577。
- 另有 4 个键只改了 zh、en / fr 没动(`guide.title`、`tour.step15.body`、`tour.step23.body`、`tour.step37.body`)。en / fr 要对照的 zh 变了,所以一并列入。
- 合计 30 条。其中 `guide.title` / `guide.rule2` / `guide.rule9` 是红字生成物,已按同一方式在 frReview 的【J153 轮】【J149 轮】复核过,这里只列出、不重复核;本轮实核 27 条。
- 各条的来源 PR 是这一段里改过该键的那次合并;下面按合并先后排列。

### 判据

- en / fr 与 zh 说的是不是同一件事:意思、极性、数字与单位、要用户做的动作、引到的钮名或宿主界面名。
- en / fr 本身是否自然:语法、法语里的性数配合与习惯说法。
- fr 标点按本仓惯例:`;` `:` `!` `?` 前与 `« »` 内侧用普通空格。这一轮用脚本数过(改后的字典):fr 全部 577 条里,含「普通空格 + `;` `:` `!` `?`」的 120 条,含不换行空格(U+00A0 / U+202F)的 0 条;本轮列出的 30 条逐条查过,都合这个惯例。
- 占位符:30 条的 en / fr 占位符集合都与 zh 相同(脚本比对)。
- zh 一个字没改(本轮的范围只到 en / fr)。

### 结果

改了 5 个键、8 个值:en 3 个(`out.master.writeConfirm`、`out.master.writeConfirm.follow`、`tour.step23.body`),fr 5 个(同样那 3 个键,加上 `tracks.colLegend`、`banner.liveReEnable`)。其余 22 条实核结论为一致、未改。每个改动值在 web 冒烟里有一格断言;把 8 个值逐个改回核前的写法,各自只让自己那一格变红。

### 逐条

#### 1. `tour.step24.title`(改动,#301)

- zh:参与音量调节
- en(核前):Volume participation
- fr(核前):Participation volume
- 结论:一致,未改。zh「参与音量调节」是开关名;en / fr 与 Tab2 列头 `tracks.colVolPart`(J127 轮已核)同词。
- 改后:无

#### 2. `tour.step24.body`(改动,#301)

- zh:独立开关:该轨是否参与音量平衡计算(默认开),与主唱锁 / Lead Select 不联动。
- en(核前):Independent switch: whether this track joins level balancing (on by default); not linked to Lead Lock / Lead Select.
- fr(核前):Interrupteur indépendant : si la piste entre dans l'équilibrage du volume (activé par défaut) ; non lié au verrou lead / Lead Select.
- 结论:一致,未改。「该轨是否参与音量平衡计算」「默认开」「与主唱锁 / Lead Select 不联动」三处 en / fr 都对得上;fr「si la piste entre dans…」与 `tour.step25.body` 同一句式。
- 改后:无

#### 3. `set.centerSlot.note`(改动,#301)

- zh:主唱锁与 Lead Select 之外的兜底规则;不影响各轨是否参与音量调节。
- en(核前):Fallback rule beyond Lead Lock and Lead Select; it does not affect whether each track joins volume adjustment.
- fr(核前):Règle de repli au-delà du verrou lead et de Lead Select ; sans effet sur la participation de chaque piste à l'ajustement du volume.
- 结论:一致,未改。en「whether each track joins volume adjustment」、fr「la participation de chaque piste à l'ajustement du volume」说的都是「各轨是否参与音量调节」这个开关,旧的「音量豁免」说法已不在。
- 改后:无

#### 4. `toast.sidecarSwitched`(改动,#301)

- zh:采集数据已超过 8MB,已转存外部文件——发给他人需重新采集
- en(核前):Capture data exceeded 8 MB and was moved to an external file — anyone you send the project to will need to capture again
- fr(核前):Les données de capture ont dépassé 8 Mo et ont été déplacées dans un fichier externe — toute personne à qui vous envoyez le projet devra refaire la capture
- 结论:一致,未改。zh「发给他人需重新采集」省了主语,en / fr 都落到「收到工程的人要重新采集」,与原意(外部文件不随工程走)一致。这条 toast 目前在页面里恒隐藏(见 `web/output/app.js` 的注释),不上屏。
- 改后:无

#### 5. `master.step2.desc.noData`(改动,#292)

- zh:当前范围内没有可用的采集数据(没连上 Input 的通道不计入)——调整范围、先采集,或连上 Input
- en(核前):No usable captured data in the current range (channels without a connected Input are not counted) — adjust the range, capture first, or connect the Input
- fr(核前):Aucune donnée capturée exploitable dans la plage actuelle (les canaux sans Input connecté ne comptent pas) — ajustez la plage, capturez d'abord ou connectez l'Input
- 结论:一致,未改。括注「没连上 Input 的通道不计入」与三个处置(调整范围 / 先采集 / 连上 Input)en / fr 都齐。
- 改后:无

#### 6. `analyze.refused`(改动,#292)

- zh:所选范围没有可用的采集数据(没连上 Input 的通道不计入),先采集这段或连上 Input,或换一个有数据的范围再重分析
- en(核前):No usable captured data in the selected range (channels without a connected Input are not counted) — capture this part or connect the Input, or pick a range that has data
- fr(核前):Aucune donnée capturée exploitable dans la plage choisie (les canaux sans Input connecté ne comptent pas) — capturez cette partie ou connectez l'Input, ou choisissez une plage qui en contient
- 结论:一致,未改。en / fr 没有译 zh 的「先」与句尾「再重分析」,不改意思(这句就是重分析被拒时弹的);括注与三个处置都齐。
- 改后:无

#### 7. `toast.recaptured`(新增,#300)

- zh:已重采集 {s}s,建议重分析该范围
- en(核前):Re-captured {s} s; re-analyzing this range is recommended
- fr(核前):{s} s re-capturées ; il est conseillé de ré-analyser cette plage
- 结论:一致,未改。fr「re-capturées / ré-analyser」带连字符,与现有钮名 `wave.btnRecapture`「Re-capturer la sélection」、`wave.btnReanalyze`「Ré-analyser la sélection」写法相同,本轮不单改这一条(见文末「未改的疑点」第 1 条)。
- 改后:无

#### 8. `toast.recapturedGoto`(新增,#300)

- zh:立即重分析
- en(核前):Re-analyze now
- fr(核前):Ré-analyser maintenant
- 结论:一致,未改。与 `wave.btnReanalyze` 同词。
- 改后:无

#### 9. `curve.shapeGroup`(新增,#315)

- zh:形状
- en(核前):Shape
- fr(核前):Forme
- 结论:一致,未改。单选组的读屏名,与 `curve.shape.*` 同族。
- 改后:无

#### 10. `curve.sideGroup`(新增,#315)

- zh:方向
- en(核前):Direction
- fr(核前):Direction
- 结论:一致,未改。与 `curve.announcePointDir` 里的 direction 同词。
- 改后:无

#### 11. `tracks.manualOverwriteConfirm`(改动,#302)

- zh:将以固定值替换该轨(当前版本)全部分段的这一项,另一项保留原曲线,可撤销
- en(核前):This sets this control to a fixed value on all analyzed segments of this track (current version); the other control keeps its curve. Undoable.
- fr(核前):Fixe cette commande à une valeur constante sur tous les segments analysés de cette piste (version actuelle) ; l'autre commande garde sa courbe. Annulable.
- 结论:一致,未改。「这一项 / 另一项」en 译作 this control / the other control,fr 译作 cette commande / l'autre commande,指 pan 与 vol 两维中的一维,与 `web/output/tab-tracks.js` 的行为说明一致;「保留原曲线」译作 keeps its curve / garde sa courbe。
- 改后:无

#### 12. `guide.rule2`(改动,#320)

- zh:SCVB Input 必须插在人声轨插件链的最后一格;SCVB Output 必须插在总线的第一格。位置不对会破坏 DAW 的处理顺序假设;各宿主对这一格的具体叫法见 `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md`。(ADR-002 / J45)
- en(核前):SCVB Input must sit in the last slot of the vocal track's plugin chain; SCVB Output must sit in the first slot of the bus. Any other position breaks the processing-order assumption SCVB relies on; for what each host calls that slot, see `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md`. (ADR-002 / J45)
- fr(核前):SCVB Input doit occuper la dernière case de la chaîne d'effets de la piste de voix ; SCVB Output doit occuper la première case du bus. Toute autre position casse l'hypothèse d'ordre de traitement sur laquelle SCVB repose ; pour le nom de cette case dans chaque hôte, voir `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md`. (ADR-002 / J45)
- 结论:红字生成物;已在 frReview【J149 轮】复核(prod 地址替换),本轮不重复。
- 改后:无

#### 13. `guide.rule9`(改动,#320)

- zh:看到"时间线缺口 / 重叠"警告时,不要继续导出。先按 `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md` 的通用坑清单排查路由,警告计数不归零就说明有轨的音频没被正确接管。
- en(核前):Do not carry on exporting while a "timeline gap / overlap" warning is showing. Work through the common-pitfalls list in `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md` to check your routing first: for as long as the warning count refuses to fall back to zero, some track's audio is not being picked up correctly.
- fr(核前):Ne poursuivez pas l'export tant qu'un avertissement « trou / chevauchement de timeline » est affiché. Vérifiez d'abord votre routage à l'aide de la liste des pièges courants de `https://github.com/synchain-oss/scvb/blob/prod/docs/DAW_COMPATIBILITY.md` : tant que le compteur d'avertissements ne retombe pas à zéro, l'audio d'une piste n'est pas correctement pris en charge.
- 结论:同上一条(frReview【J149 轮】)。
- 改后:无

#### 14. `banner.stateNotRestored`(新增,#307)

- zh:段表没能恢复,原数据会原样保留
- en(核前):The segment table could not be restored; the original data will be kept as is
- fr(核前):La table des segments n'a pas pu être restaurée ; les données d'origine seront conservées telles quelles
- 结论:一致,未改。
- 改后:无

#### 15. `guide.title`(仅 zh 改,#319)

- zh:必读:SCVB 的九条使用规则,违反其中任何一条都会导致静音、声像位置错误或分析失效。
- en(核前):Must read: SCVB's nine usage rules. Breaking any one of them causes silence, wrong panning, or failed analysis.
- fr(核前):À lire : les neuf règles d'utilisation de SCVB. En enfreindre une seule entraîne silence, panoramique erroné ou analyse échouée.
- 结论:红字生成物,zh 在 J153 改过;已在 `docs/hard-rules.i18n.json` 的 frReview【J153 轮】复核,本轮不重复。
- 改后:无

#### 16. `tour.step15.body`(仅 zh 改,#319)

- zh:每条声像曲线由控制点构成:双击任意位置添加点,拖动调整角度与增益,双击删除;每个点可选钟形 / 搁架 / 切除(带 6–24 dB/oct 斜率),最多 16 点
- en(核前):Each pan curve is built from control points: double-click anywhere to add a point, drag to adjust angle and gain, and double-click a point to delete it. Each point can be a bell / shelf / cut node (6–24 dB/oct slope); up to 16 points.
- fr(核前):Chaque courbe de panoramique est constituée de points de contrôle : double-cliquez n'importe où pour ajouter un point, faites glisser pour ajuster l'angle et le gain, et double-cliquez sur un point pour le supprimer. Chaque point peut être cloche / plateau / coupe (pente 6–24 dB/oct), jusqu'à 16 points.
- 结论:一致,未改。J153 删掉了 zh 的「选中后」;en / fr 在 J127 轮就已改成「双击一个点将其删除」,本来没有「先选中」。
- 改后:无

#### 17. `tour.step25.body`(改动,#319)

- zh:该轨是否参与声像重分布(默认开,含立体声轨);关掉后仍参与音量平衡。
- en(核前):Whether this track joins pan redistribution (on by default, stereo tracks included); when off, it is still level-balanced.
- fr(核前):Si la piste entre dans la redistribution du pan (activé par défaut, pistes stéréo comprises) ; une fois désactivé, l'équilibrage du volume est conservé.
- 结论:一致,未改。「默认开,含立体声轨」「关掉后仍参与音量平衡」en / fr 都对得上。
- 改后:无

#### 18. `tour.step23.body`(仅 zh 改,#319)

- zh:把两条轨配成一对,配对的两轨声像联动、作为一个整体移动;同一配对的两轨行首显示同色圆点。
- en(核前):Pair two tracks; their pan is linked and the pair moves as one. Same pair shows the same colored dot at the row head.
- fr(核前):Appairez deux pistes ; leur panoramique est lié et la paire se déplace comme un tout. Même paire = même point coloré en tête de ligne.
- 结论:**改 en、fr**。J153 把 zh 的主语改成「同一配对的两轨」;en「Same pair shows the same colored dot at the row head.」、fr「Même paire = même point coloré en tête de ligne.」都没有「两轨」这个主语,en 这句语法也不通。改后主语与 zh 对齐。
- 改后 en:Pair two tracks; their pan is linked and the pair moves as one. Both tracks of a pair show the same colored dot at the start of their rows.
- 改后 fr:Appairez deux pistes ; leur panoramique est lié et la paire se déplace comme un tout. Les deux pistes d'une même paire affichent le même point coloré en tête de ligne.

#### 19. `tour.step37.body`(仅 zh 改,#319)

- zh:三步工作流 + 九条重要提示;请逐条读一遍,违反任何一条都会导致静音或声像位置错误。
- en(核前):Three-step workflow + nine important notes; please read each one. Breaking any of them causes silence or wrong panning.
- fr(核前):Flux en trois étapes + neuf notes importantes ; lisez-les une par une. En enfreindre une seule cause silence ou panoramique erroné.
- 结论:一致,未改。en / fr 本来就写 wrong panning / panoramique erroné(J153 就是让 zh 跟上它们)。
- 改后:无

#### 20. `tracks.colLegend`(改动,#319)

- zh:音量＝参与音量调节,该轨是否进音量平衡计算(默认开) · 声像＝参与自动声像,该轨是否进声像重分布(默认开,含 stereo 轨;关掉后仍参与音量平衡) · 冻结P/V＝结果照算但不再驱动,旋钮解锁为手动(两开关共用一个每轨自动化参数)
- en(核前):Vol = volume participation, whether this track joins level balancing (on by default) · Pan = auto-pan participation, whether it joins pan redistribution (on by default, stereo included; still level-balanced when off) · Freeze P/V = still analyzed but no longer driven; knob/fader unlock to manual (both switches share one per-track parameter)
- fr(核前):Vol = participation volume, si la piste entre dans l'équilibrage (activé par défaut) · Pan = participation au pan auto, si la piste entre dans la redistribution (activé par défaut, stéréo compris ; équilibrage conservé une fois désactivé) · Gel P/V = toujours analysé mais plus piloté ; potentiomètre/fader déverrouillés en manuel (les deux interrupteurs partagent un même paramètre par piste)
- 结论:**改 fr**。zh 写「含 stereo 轨」;fr「stéréo compris」里的 compris 放在名词后面要与名词配合(la stéréo ⇒ comprise),而且 stéréo 单用读成「立体声」,不是「立体声轨」。改成与 `tour.step25.body` 同一说法「pistes stéréo comprises」。en「stereo included」可读,未改。
- 改后 fr:Vol = participation volume, si la piste entre dans l'équilibrage (activé par défaut) · Pan = participation au pan auto, si la piste entre dans la redistribution (activé par défaut, pistes stéréo comprises ; équilibrage conservé une fois désactivé) · Gel P/V = toujours analysé mais plus piloté ; potentiomètre/fader déverrouillés en manuel (les deux interrupteurs partagent un même paramètre par piste)

#### 21. `banner.reaperKeepOpen`(新增,#324)

- zh:REAPER:写入自动化期间请保持本插件窗口打开——窗口关着时 REAPER 可能不写入自动化
- en(核前):REAPER: keep this plug-in window open while writing automation — REAPER may not write automation while the window is closed
- fr(核前):REAPER : gardez la fenêtre de ce plug-in ouverte pendant l'écriture de l'automation — REAPER peut ne pas écrire l'automation quand la fenêtre est fermée
- 结论:一致,未改。
- 改后:无

#### 22. `banner.reaperPrintNote`(新增,#324)

- zh:REAPER:若写完后没有录到自动化,请在 Preferences → Plug-ins → VST → VST compatibility 中把 Parameter automation notifications 设为 process all notifications
- en(核前):REAPER: if no automation was recorded after writing, set Parameter automation notifications to process all notifications under Preferences → Plug-ins → VST → VST compatibility
- fr(核前):REAPER : si aucune automation n'a été enregistrée après l'écriture, réglez Parameter automation notifications sur process all notifications dans Preferences → Plug-ins → VST → VST compatibility
- 结论:一致,未改。REAPER 的首选项路径与选项名三语都保留英文原文(REAPER 界面是英文),与 `docs/DAW_COMPATIBILITY.md` 逐字一致(`smoke-host-hints.mjs` 有断言)。
- 改后:无

#### 23. `banner.liveReEnable`(新增,#324)

- zh:Live:写入已结束。Re-Enable Automation 按钮亮起属正常现象,点击它即可恢复读取自动化
- en(核前):Live: writing has finished. The Re-Enable Automation button lighting up is expected — click it to resume reading automation
- fr(核前):Live : l'écriture est terminée. Le bouton Re-Enable Automation allumé est normal — cliquez dessus pour reprendre la lecture de l'automation
- 结论:**改 fr**。zh「按钮亮起属正常现象」说的是「它亮着是正常的」;fr「Le bouton Re-Enable Automation allumé est normal」字面是「亮着的那个按钮是正常的」,法语读着别扭。改成「Il est normal que le bouton Re-Enable Automation soit allumé」。en「…button lighting up is expected」准确,未改。
- 改后 fr:Live : l'écriture est terminée. Il est normal que le bouton Re-Enable Automation soit allumé — cliquez dessus pour reprendre la lecture de l'automation

#### 24. `ch.claimFailed.unavailable`(新增,#328)

- zh:未能连接:插件间通信用的内存段打不开。请重新选择;仍不行请重启宿主后再试
- en(核前):Could not connect: the shared memory the plug-ins communicate through could not be opened. Select again; if it still fails, restart the host and retry.
- fr(核前):Connexion impossible : la mémoire partagée par laquelle les plug-ins communiquent n'a pas pu être ouverte. Sélectionnez à nouveau ; en cas de nouvel échec, redémarrez l'hôte puis réessayez.
- 结论:一致,未改。
- 改后:无

#### 25. `ch.claimFailed.abiMismatch`(新增,#328)

- zh:未能连接:两端 SCVB 版本不匹配——请把两个插件升到同一版本
- en(核前):Could not connect: SCVB version mismatch. Update both plug-ins to the same version.
- fr(核前):Connexion impossible : versions SCVB incompatibles. Mettez les deux plug-ins à la même version.
- 结论:一致,未改。与 `banner.abiMismatch`(J127 轮已核)同一说法。
- 改后:无

#### 26. `master.rangeBars`(新增,#325)

- zh:小节 {x} → {y}
- en(核前):Bars {x} → {y}
- fr(核前):Mesures {x} → {y}
- 结论:一致,未改。小节 = Bars / Mesures。
- 改后:无

#### 27. `master.barsMeterNote`(新增,#325)

- zh:拍号有变化,小节号为估算值
- en(核前):Time signature changed — bar numbers are estimates
- fr(核前):Le chiffrage de mesure a changé — numéros de mesure estimés
- 结论:一致,未改。「拍号有变化」在 `web/output/host-tempo.js` 里的语义是「本次会话观察到拍号变过」,en「Time signature changed」、fr「Le chiffrage de mesure a changé」都对。
- 改后:无

#### 28. `master.rangeSecondsNote`(新增,#325)

- zh:未读到宿主速度与拍号,范围按秒显示,±4 为 4 秒
- en(核前):No host tempo or time signature yet — range shown in seconds, ±4 = 4 s
- fr(核前):Tempo et chiffrage de l'hôte pas encore reçus — plage en secondes, ±4 = 4 s
- 结论:一致,未改。zh「未读到」en / fr 译作 yet / pas encore。fr 在「Tempo et chiffrage」里单用 chiffrage 可读(同一行位置的 `master.barsMeterNote` 写全了 chiffrage de mesure),不为它把这行加长:RANGE 卡被挤压时最先裁掉的就是这行注。
- 改后:无

#### 29. `out.master.writeConfirm`(改动,#337)

- zh:点「知道了,开始」后才写入自动化 {v}(点之前只试听、不写) · 范围 {x}–{y} · 30 条轨道;若 DAW 侧已激活 Latch/Write,播放本范围将覆盖该范围已有自动化;未激活则仅试听、不保存
- en(核前):Write automation {v} starts only when you press “Got it, start” (monitoring only until then) · range {x}–{y} · 30 tracks. If Latch/Write is active in your DAW, playing this range will overwrite existing automation there; if not active, this is monitoring only.
- fr(核前):L'écriture d'automation {v} ne démarre qu'après « Compris, démarrer » (écoute seule d'ici là) · plage {x}–{y} · 30 pistes. Si Latch/Write est actif dans votre DAW, la lecture de cette plage écrasera l'automation existante ; sinon, écoute seule.
- 结论:**改 en、fr**。意思一致。en 开头「Write automation {v} starts only when…」读成祈使句,改成动名词主语「Writing automation {v} starts only when…」;fr「ne démarre qu'après « Compris, démarrer »」把钮名直接接在 après 后面,少了「点」这个动作,补成「qu'après un clic sur « Compris, démarrer »」。引的钮名与 `master.writeConfirm.ok` 三语逐字一致。
- 改后 en:Writing automation {v} starts only when you press “Got it, start” (monitoring only until then) · range {x}–{y} · 30 tracks. If Latch/Write is active in your DAW, playing this range will overwrite existing automation there; if not active, this is monitoring only.
- 改后 fr:L'écriture d'automation {v} ne démarre qu'après un clic sur « Compris, démarrer » (écoute seule d'ici là) · plage {x}–{y} · 30 pistes. Si Latch/Write est actif dans votre DAW, la lecture de cette plage écrasera l'automation existante ; sinon, écoute seule.

#### 30. `out.master.writeConfirm.follow`(改动,#337)

- zh:点「知道了,开始」后才写入自动化 {v}(点之前只试听、不写) · 范围 = 全部已分析区域(全曲跟随,共 {n} 段 · 合计 {t}) · 30 条轨道;若 DAW 侧已激活 Latch/Write,播放已分析区域将覆盖其已有自动化;未激活则仅试听、不保存
- en(核前):Write automation {v} starts only when you press “Got it, start” (monitoring only until then) · range = all analyzed areas (follow, {n} segments · total {t}) · 30 tracks. If Latch/Write is active in your DAW, playing analyzed areas will overwrite existing automation there; if not active, this is monitoring only.
- fr(核前):L'écriture d'automation {v} ne démarre qu'après « Compris, démarrer » (écoute seule d'ici là) · plage = toutes les zones analysées (suivi, {n} segments · total {t}) · 30 pistes. Si Latch/Write est actif dans votre DAW, la lecture des zones analysées écrasera l'automation existante ; sinon, écoute seule.
- 结论:**改 en、fr**,改法同上一条。
- 改后 en:Writing automation {v} starts only when you press “Got it, start” (monitoring only until then) · range = all analyzed areas (follow, {n} segments · total {t}) · 30 tracks. If Latch/Write is active in your DAW, playing analyzed areas will overwrite existing automation there; if not active, this is monitoring only.
- 改后 fr:L'écriture d'automation {v} ne démarre qu'après un clic sur « Compris, démarrer » (écoute seule d'ici là) · plage = toutes les zones analysées (suivi, {n} segments · total {t}) · 30 pistes. Si Latch/Write est actif dans votre DAW, la lecture des zones analysées écrasera l'automation existante ; sinon, écoute seule.

### 未改的疑点(不在本轮范围,另报)

1. fr 字典里「重新采集 / 重新分析」两种写法并存:钮名与导览用带连字符的 Re-capturer / Ré-analyser,横幅用 recapture / recapturer。法语的标准拼写不带连字符(recapturer、réanalyser)。要统一就得动本轮范围以外、J127 轮已核过的词条,本轮不动。
2. `banner.liveReEnable` 三语都引 Live 按钮的英文原名 Re-Enable Automation(`smoke-host-hints.mjs` 断言它在)。Live 有法语与中文界面,界面语言不是英文的用户看到的按钮名会不同。要改得先对照 Live 各语言界面里的真名,本机核不了。
3. `toast.recaptured` 的 `{s}` 由 `fmtRecapSeconds` 按小数点格式填(如 3.2),法语界面没有换成逗号。这是填值代码的事,不是词条值;其余带数字占位符的 fr 词条同理。
4. `ch.claimFailed.unavailable` 的 en / fr 长 151 / 189 个字符,放在 4 秒后自动收起的 toast 里(zh 是 36 个字)。读不读得完属于交互设计,本轮不改。

### 这一轮不验什么

- 判据只是「en / fr 与 zh 说的是不是同一件事」加可读性。zh 本身过时、三语跟着一起错的那一类核不出来(与 J127 轮同样的限制)。
- 界面长度:改动的 5 个键都放在会折行的容器里(导览气泡、Tab2 图例段落、横幅、确认条),没有单行截断的位置;本轮没有逐语言跑页面级排版测量。
- 这是经授权的 AI 核对,不是法语母语者的人工审校。
