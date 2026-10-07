# 新 session 提示词

## 当前WIP更新：64个零权重回退来源，新组仍待完整验证

第二组ON350原Gather因64 invalid frame stamp拒绝，原Fusion/driver通过；OFF350和OFF480原native门通过，350480 SCstage尚未完成。64像素实际diffuse RGB0/confidence0、glossy保留既有回退，原失败data不改。现已显式补全实际bestRefl单probe源及unused blend0低频数学、锁16B ModeCB ABI；未改原门/曝光/射线/旧SC选择，未缩小过滤范围。完整serial/Unit7+5 PASS；第三组新binary `/tmp/sc-gather-geometry-candidate-zero-source` OFF350运行中，必须完成新同binary OFF/ON350480及输出保持/视觉原门核验，方能决定保留或恢复。[当前WIP](../reports/sc-resolve-2026-10-03/gather-geometry-candidate/WORK_IN_PROGRESS.md)、[实际handle](../reports/sc-resolve-2026-10-03/gather-geometry-candidate/progress.json)。当前源仍有候选，不引用573恢复为当前完成。

## 当前进行中：Gather动态几何资格候选（573之后，尚未保留／回退）

五现有源＋新helper＋两child SPV已实现opt-in同binary OFF/ON；原231SPV SHA保持，既有dirty保留。prototype350真indirect右−18.76%、左−21.25%、墙−1.10%，有暗廊收敛但未完成视觉目标。OFF原native门／driver／SC PASS；ON原Gather因误写default_main_preserved=false拒绝，原Fusion PASS，prototype不当合格候选。元数据按原“aux保留所选primary”语义修正并显式标主candidate/application，未改门／旧捕获；fresh valid组正从OFF350重捕获，完整serial/Unit7+5已通过。当前源码尚有候选修改，不能把下方573的恢复SHA当当前完成状态。须poll实际live handle，完成同有效binary350480／原门／视觉HDR，再保留或恢复并回归。[当前工作与入口](../reports/sc-resolve-2026-10-03/gather-geometry-candidate/WORK_IN_PROGRESS.md)、[状态](../reports/sc-resolve-2026-10-03/gather-geometry-candidate/progress.json)。

## 573最新：当前Gather原生连接资格与读取补证，未应用GI

- 同completed350实际2531 tap／378 active receiver，私有5818查询保留原20µm/64stack；query GPU echo、七生产buffer前后、七geometry/material源与350逐位。private模块/native bind/11 exact supplied writes/ordered dispatch PASS；actual albedo LOD0 alpha1与quad witness仅局部资格，不冒充所有mip或完整opacity/transport。
- 原Gather main355 dispatch／唯一350 aux同command顺序、所选plane全部rewrite及SH17/origin19等原native来源补齐；882最近actual SH/origin→current Gather逐位，origin状态全1。bindings3/7/8/9/10/11只有存在记录无独立sourceexport；无descriptor物理内存或每帧CB writer时间戳，早期history祖先仍unknown。更正565“Gather创建早于logger”推断：实际logger whitelist遗漏Gather；首次proof350证书拒绝，补临时whitelist并重编、重新捕获proof350cert，不沿用旧证书。
- native alpha1原guard2341中CPU首面ID一致2333，8亮斑记录ID不同，整体独立ID门FAIL_8_ID_MISMATCH保留UNKNOWN；t误差≤2.23544µm不能代ID门/解决tie。右biased447实际tap有136 alpha1/ID一致/前向；154 miss、157 numerical未知。实际camera receiver首面52≤2mm，联合first gap>.152m后106 tap可测。signed sample L0右25.205%、左20.914%、墙2.091%、floor .534%，不是全ROI/Fusion能量、漏光率、删除收益或root；原CPU293不可直接裁权重。
- 临时两C++／新helper/SPV全部恢复，retained515的13source+231SPV共244SHA保持既有dirty。完整DebugVulkan serial、Unit7+5、新final V350480/Metal≥65无fault-VUID、原SCstage PASS。六有效V原Gather全帧/六ROI1%及Fusion2halfULP PASS，四350四SC/六main逐位/GPU112/13/80 PASS；旧ratio六FAIL，P0缺失null/NOT_RUN历史FAIL独列。Metal仅API回归；固定EV1.8546及真实indirect归档，灰亮/颜色/层次未改善，目标未完成。
- 网页573capture47段保留原46；414raw145unique/99输入新目录SHA重放analysis/Gather-driver/SH-origin JSON及CPUquery-first raw逐位、原native门重放PASS、整体ID FAIL/8unknown保留；524本轮副本核SHA清理statvfs+4365139968B含并发，非独占回收，browser_QA false。[本轮证据](../reports/sc-resolve-2026-10-03/native-gather-connection-proof/README.md)。
- 下一步直接窄动态几何资格Gather visibility同binary/SPV OFF/ON：实际camera receiver≤2mm、native first/alpha1/front/原20µm、gap>.152m才测，unknown原结果保持；不得ROI/固定106 ID掩码裁tap，不调floor/N/SH/history/曝光，不减少原射线/更新，不重推discard continuation。视觉/HDR及原350480门失败即回退；whole ID FAIL不能改名全PASS。

## 565只读追加：当前Gather实际tap的连线几何，尚非漏光根因

- 从565 certificate350按SHA恢复21输入，固定六ROI384 actual receiver／378 active near／2531实际accepted tap；原native ID／regular-low weight／N／world／confidence／实际relocated或名义origin，不混analytic history query或CPU可能角点。完整262267三角native positions/BVH a/e1/e2/32B mapping owner/identity/material/cluster numeric核验。
- CPU64 tmin20µm／target前2mm到unbiased及shader .15m biased receiver，unknown0；右64receiver447tap，biased293前向flags0/mapping0交叉，其中291 first距目标>.152m，receiver nearest均≤2mm。原无bias319交叉，不能代原shader路径；右293项signed L0代数和59.1869%，非全ROI／Fusion energy／物理漏光率／删tap收益或根因。当前新连线native GPU／visible receiver triangle／texture alpha／physical opacity证书仍缺，不能凭CPU几何或flags0改权重。
- 无新Engine/shader/GPU/capture/HDR/回归或视觉收益，565 EV图灰亮差距仍在；565新Vulkan与561完整恢复回归独立分列，P0缺失／旧ratioFAIL保持。网页565capture46段保留45；29raw27unique／21新目录SHA重放JSON与六derived raw/bin逐位，58本轮副本清理statvfs+500396032B非独占回收，244/current binary/assets保持，browser_QA false。[当前Gather连接](../reports/sc-resolve-2026-10-03/native-gather-current-connection/README.md)。
- 下一步为当前293连接补native material/alpha与receiver身份，先private-only witness核原Gather读取／输出保持，再窄几何资格同binary开关对照，不先裁visibility floor／N／history、删tap或压暗。目标未完成。

## 565最新：实际最近 writer→native Gather，350／480／1024积累未改善灰亮

- 未改Engine／shader／曝光／光照／128射线／更新预算／history；retained515的244源SPV与当前binary/assets SHA保持。四次新固定EV1.8546捕获与真实indirect HDR保留。current350缺driver prefix的producer证书拒绝，渲染与原native门有效；三次first-rho application0只读控制补证，非新GI候选。
- 每次882完整最近writer，epoch312–350／444–480／987–1024；fresh均220probe／28160ray。原projection／alpha／clamp相对容差PASS，actualAfter vec4→当前native flat near SH位相同，再接实际Gather ID／N／regular-low／confidence。producer driver ordered pairs349／479／1023；不是新增Gather driver地址证书或完整物理opacity／早期history证书。
- 右暗廊实际19 near probe，主要507最近／上一writer350/349→480/479→1024/1023，521为324/288→468/432→1008/972，409为350/349→480/479→1024/1023。350右L0 signed .335491747，latest input history .328432679，最新positive-distance .006976557、exhausted .000082532；旧ancestry仍unknown，旧positive不能从distance再判SC／screen／analytic；单次IIR约98%不证明积累bug。
- 真indirect右350/480/1024 .032975845/.033637296/.034670524（1024 +5.139%），左.045957826/.046919968/.050153062（+9.128%）；图中灰亮、颜色和空间层次差距仍在，非收益／收敛／漏光根因证书。无prefix与control350的SH/history/rays/list/depth/view位保持，normal及最终HDR不位等，不能当全图隔离等价。
- 新证书350/480原SCstage PASS；四新native Gather1%/Fusion2halfULP PASS，两350四SC／六主query／GPU112/13/80 PASS；旧ratio四FAIL/P0缺失NOT_RUN历史FAIL单列。无引擎修改，未新跑build／Unit／Metal，561回退历史独立。网页565capture45段保留44；333raw179unique/262输入新目录三analysis+CSV+15系数逐位／四native门重放PASS；618副本SHA核验清理，statvfs+5139996672B非独占回收，browser_QA false。[当前writer与积累证据](../reports/sc-resolve-2026-10-03/ddgi-native-latest-writer/README.md)。
- 下一步补当前实际高贡献probe与receiver的几何／可见性资格及更早writer来源，资格闭合再做窄同binary开关；不凭局部遮挡、positive或IIR比重调alpha／裁SH／抑制radiance，不重复已discard的全局distance／normal／Eπ／continuation。目标未完成。

## 561只读追加：native history SH阶次／颜色和方向符号，未改渲染

- 536 native350的45原raw逐SHA恢复；18538 actual analytic／9486 grid内、37828 native槽位，ID/weight/N/SH与同次history逐位，原query/stamp及N=-RD保持。九band→实际history/rho/sky+ray、原距离加权/128ray投影/alpha/clamp/published nearSH→native current Gather原带尺度门全PASS（history max1.1444e-5、projection2.6703e-5、writer6.1035e-5，不称所有<1e-5）。
- actual history反弹signed ray Y和L0+7674.104、L1−2693.625、L2+10197.332；当前Gather右暗廊preclamp Y的输出阶次 .335492/.044290/.013097，左 .853740/.008863/.066972。它们与输入history阶次经本帧writer后的 .00103852/−.00006044/.00153131（右）不同；旧fresh history .16466573、未更新probe祖先未知 .22479677仍保留。不是postclamp/Fusion物理份额、删除反事实或灰亮root。
- native history N=−RD，native writer投影=+RD逐位；SH basis奇偶原门max8.34465e-7。实际normalized weights/rho/distance/alpha和冻结actual clamp scale的CPU32同索引transfer：L0/L2全部非负、L1全部非正，权重和max1.19209e-7。不同空间源、跨mode项、更改clamp后的feedback、真实surface/N/opacity资格尚未闭合；不因符号重推discard法线/Eπ/距离或裁SH。
- 无新GPU/capture/HDR/Engine/候选或视觉收益；使用原536固定EV图与trueindirect，当前retained244/两个binary/同资产SHA保持。新Unit/V/Metal/SCstage为NOT_RUN_READ_ONLY，561回退历史PASS独立；旧ratioFAIL/P0缺失未验分列。网页capture仍561，44段保留原43；55raw45unique/45 input新目录重放analysis+10derived raw逐位，110本轮副本SHA核验清理、statvfs+936931328B非独占回收；browser_QA false。[native阶次与实际链](../reports/sc-resolve-2026-10-03/ddgi-native-history-bands/README.md)。
- 下一步追实际暗部native Gather接受tap中的dominant旧SH，连接latest与中间writer的方向／颜色／surface支持和读回身份，保留更早祖先UNKNOWN。不得把本帧小项或L0计数直接当全部历史原因，也不重复扩大无支持stop继续march。目标未完成。

## 561最新：初始全零路径继续跨mixed stop，视觉／原门失败，已恢复515

- 两源／新helper／两SPV临时开关只用于原SC及screen失败、初始manual scalar及八角全零且原band+.002m无几何支持的analytic路径；途中也拒绝mixed无支持stop，64额外步沿用原步进／bisection，unknown／越域／limit保留旧结果。原16、旧card/cacheT、N=-RD／rho、实际28160射线与更新不变，不以nearest当步长或首面。
- 第二组同binary／231SPV／assets OFF-ON350480、native CB7=0/1；接受2894／3267点。双帧CPU64 old外／new内band资格PASS，但零个新t在CPU first20µm内（350无2mm点，480仅1），不是实际surface／opacity或物理root证书。真实indirect HDR右+12.873%／+16.921%、左+4.018%／+5.736%、direct_patch的indirect−14.987%／−19.242%、墙−4.865%／−4.142%；右墙／拱顶灰亮无改善，ON原SCstage FAIL（350 direct_patch error delta+3.207>2），OFF PASS，DISCARD。
- 原source4／query18全16在19072／20736共同实际射线逐位；列表不同不得dense逐ray归因。350同一次previous SH新值CPU32原容差全PASS/max1.4305e-5，旧位置仅数学反事实：2292增／598减／4同，signed ray Y差和+2942.794，非screen能量／物理因果；新位置更近粗带不保证GI收益。四次candidate Vk模块SHA／layout绑定28–30／dispatch身份核验，仍无独立新zc texel值、GPU tetra ID／weight、native opacity证书。
- 首组四次ON CB7=0，且旧C++layout缺采样binding28–30出现layout-07988 VUID，全部REJECTED；之前首组OFF350通过的进度说法已更正，native数值闭合不能替代合法pipeline。旧mtime复制导致CPP未重编，touch重编后freshness拒绝旧SPV时间戳，再build_spv＋fullserial重建才完成第二组，所有失败日志／截图／HDR独立保留。
- 233 originals完全回退／new helper删除／515 retained244 SHA保持；完整DebugVulkan serial／Unit7+5／新普通V350480／Metal≥65秒／无fault-VUID／恢复原SCstage与native原门PASS。6有效V Gather全帧六ROI1%／Fusion2halfULP，3有效350四SC／六main／GPU112-13-80 PASS。旧ratioFAIL/P0缺原基线null、历史FAIL分列。
- 网页561capture43段保留旧42，双帧固定EV1.8546图与trueindirect HDR留档；1258raw453unique、214 raw输入新目录350480分析／配对／双帧资格／350 history math／四case identity JSON关键raw逐位；1476本轮副本SHA核验清理，statvfs可用+8.50793GB非独占物理回收。browser_QA false。[候选与回退证据](../reports/sc-resolve-2026-10-03/ddgi-mixed-continue/README.md)。目标未完成。
- 下一步不要扩大该继续march资格或重推529／505／406、硬削history／sky／SH／曝光。先在实际暗部receiving sample与可核验writer链上区分粗带位置、真实surface/N/rho资格和历史SH方向／颜色贡献；保持native读回身份、同binary实际开关、原门及视觉/HDR判据，不能把更多几何支持或局部遮挡当已证root。

## 550最新：mixed实际角点合并来源，未改GI

- 549 reason32实际8472点的67776 native corner坐标/值去重41433，新现有只读AuditCacheVoxels重新载入冻结模型逐mesh合并；41433原1mm+.1%门全部PASS/max.00194645m（不冒充20µm三角证书）。fresh固定EV350 completed384³field=549 native values=cache body逐位，244源SPV/binary231SPV/资产保持。
- 33919unique winner下界／7514有效mesh sample，20760zero全部只有下界零候选，无mesh零候选/无有效mesh min缺失。全部candidate/tie保留；67776 active corners中57205下界winner/34012仅下界零/67740有效mesh min≥.45；5788点全部winner下界、2684含mesh，8466全部八角有效min≥.45，余6点36角低于门不能混用。不是GPU winner/render owner/material/opacity/真surface或灰亮root证书。
- 新V350≥65 fixedEV1.8546/ToneAdapt0/raySample0/zero-mode2/原view字段exact；四SC/旧六main/GPU112/13/80/native Gather全帧六ROI1%/Fusion2halfULP PASS，旧ratioFAIL/P0缺基线单列。本轮无Engine编辑，没有新Unit/Metal/480配对SCstage；549历史完整PASS独立。首次validator把raySample mode0当zero2比较被拒绝，按字段分别0/2核验，不改环境/门。
- 网页550capture42段保留41，trueindirect HDR/新图保持，灰亮颜色层次差距未改善；新目录SHA/CSV候选重放/清理见summary，browser_QA false。下一步窄开关仅旧SC/screen失败、初始全零无几何支持范围继续跨mixed无支持stop；原16/旧card-cacheT/ray/unknown旧结果与原门保持，实际HDR再决定，不能假定更多拒绝会更好/改sky/裁history/重复529505406。[实际mixed角来源](../reports/sc-resolve-2026-10-03/ddgi-mixed-voxel-source/README.md)。目标未完成。

## 549最新：只读native mixed角点／终止原因，诊断已恢复515

- 临时两源+新helper/two SPV，CB7=2，30vec/ray guard30k≤18(N−k)不覆盖原27N original/query18全16，仅shadow545继续march且**不应用新点/t/radiance**。28160实际ray/18538analytic，原list/ray/history/analytic4/source4/query18=545OFF逐位；旧N/rho/point/stamp与解析代数原门PASS。
- status3全部8472 reason32=非零混合八角且原band+.002m无几何支持，stack/NaN unknown0；另6424初始非全零/192初始有支持/146只读合格/375field外50m/320耗尽，2609未执行因albedo10 behindcamera(2163)/12 invalidsky(446)。实际scalar中位.00471436m、step13、last−oldT .355525m，last/support点同位，本组无二分差异。
- 本次初始127432/last71480实际角点=completed384³R16F=原cache body逐位；scalar/allzero copies exact、CPU trilinear原门PASS。4实际module SHA/710 layout bind-dispatch、near audit_handle2512的set0/359/365 binding28 source_handle1333=同次readback源核对；非独立物理地址/全部far证书，不借rho metadata描述shadow算法。current32B mapping误按compact16B首次拒绝留档，修后262267 native triangle a/e1/e2/owner=current GPU positions/mapping逐位/world identity，非全meshlet/opacity证书。
- CPU64：8472 actual点nearest最小.452933m/中位2.045725m/最大5.32067m全部超原band+.002；actual ray first3202中2838前/364后/0原20µm贴面，5270无50m首面。偏差不限全零八角，解释545窄试验为何退回旧值，仍未证明mesh support winner/物理sky/遮挡/灰亮root。下一步补实际mixed角winner或对已证band无支持范围测试继续原步进，不因数量/无50m改sky/删history，真surface/N/rho和writer→Gather/Fusion仍缺。
- 233originals/new helper删除/515244 SHA恢复；fullserial DebugVulkan/Unit7+Debug5/新V350480/Metal65/恢复原SCstage/native门PASS，三V Gather全帧六ROI1%/Fusion2halfULP，两350四SC/六main/GPU112/13/80 PASS。诊断只350/无480配对，旧ratioFAIL/P0缺原基线单列。fixedEV shadow→545OFF trueindirect右0%、左+.008881%、墙−.000809%，非全图byteexact/视觉收益；灰亮差距仍在。
- 网页549capture41段保留40，408raw331unique/61input独立SHA恢复analysis/geometry/identity/first/nearest/JSON逐位，488自有副本核SHA清理/statvfs+2.831GB非独占回收。comparison已查看/browser_QA false，目标未完成。[当前mixed停步与原生身份](../reports/sc-resolve-2026-10-03/ddgi-zero-stop-source/README.md)。

## 545只读追加：152接受新点的history/位置影响

- 545同次原始输入SHA恢复，ON152接受仅updated ray .539773%；actual history仅1全零、Y和394.017883、sky和10.516921（signed ray数值和，非screen energy份额）。CPU32旧query/新native historyPos151均在grid内、1均在外，无跨界/1e−5边界歧义，非native gp证书；排除“接受点主要grid外所以无效”的具体猜测。
- 同ON before-history CPU32原tetra/.001/9SH/N/rho在新点152全部原容差PASS/max1.049e−5，再评估旧点相同history的数学反事实：99增/52减/1同、signed Y差和+167.276917，60 tetra IDs tuple变/151权重变；旧值未GPU执行，不能当物理根因/屏幕因果。共同actual source/query exact的135接受ray actualradiance差和+174.206787、sky RGB逐位，history此前反馈不同，不能叫位置独占影响。
- probe384/ray72 oldT.403384→new3.548412m，新点距几何.429170m/CPU first4.004586m；同historyY .317114→8.665812、actual新8.665814，原N−RD/rho.5。向几何靠近可采到更亮SH，命中数不保证暗部改善。继续拆status3实际终止原因/new texel operands及mesh支持域；不扩拒绝域/硬削history/nearest当首面/重推529505406。
- 本轮无Engine/shader/GPU/HDR/新视觉收益，515244SHA当前保持，新回归NOT_RUN_READ_ONLY、545历史PASS单列；网页545capture40段保留39，同次输入独立SHA重放values/slots/JSON逐位及本轮raw清理见summary。[接受点history位置数学](../reports/sc-resolve-2026-10-03/ddgi-zero-continue-energy/README.md)。目标未完成。

## 545最新：全零无几何支持停点继续march，无视觉收益，已discard

- 同candidate binary/SPV OFF350/ON350（固定EV1.8546/ToneAdapt0/raySample0/zero-mode2/≥65秒）：原SC/screen成功路径保持，失败analytic才检查原tHi全零八角+原band/.45m+2mm无几何支持并额外最多64步，未知/耗尽/逃逸保留旧结果。仅接受新点用于原history位置+t；N仍−RD/rho.5，无SC重试或光照/门/射线调整。两临时源+新helper、两analytic-rho SPV；显式shader生成后完整-j1构建、spirv-val通过，首个freshness拒绝日志保留。
- ON actual解析18798，152接受；CB7=1/stamp/ray/rho/N和原解析代数witness原门PASS。208共同actual probe/26624ray原source4/query18全部16候选逐位，非共同列表不逐ray比较，history也已受先前帧影响。CPU64最近面确认新点仍距几何.27028–.45194m、中位.44029m、0个≤2mm；151有actual CPU50m首面但0个20µm贴首面，不将原粗带支持升格为真实surface/根因。新VkShaderModule SHA/driver layout绑定28–30 write/dispatch已核，不是实际新texel值证书；继承rho.json algorithm_changed:false不认证continuation事实。
- ON/OFF whole-run真实indirect HDR右+.050611%、左+.007004%、墙−.274971%、亮斑间接−.181100%、近地+.035157%，没有暗部/颜色/层次可保留收益，旧显示ratio仍FAIL。**DISCARD**：233 originals/new helper删除、515244 SHA恢复；完整DebugVulkan/Unit7+Debug5/新恢复V350480/Metal65/恢复原SCstage/native门PASS，四V Gather全帧六ROI1%/Fusion2halfULP PASS，三350四SC/六main/GPU112/13/80 PASS。候选仅350无480配对SCstage；P0原baseline缺失null NOT_RUN/历史FAIL单列。
- 网页545capture39段保留旧540/38，固定EV对照Lumen仍右墙/拱顶灰亮、色彩与空间层次差距；新同次原输入SHA/gzip独立目录重放analysis/nearest/first/pair/JSON逐位、清理范围/statvfs见summary，不称独占回收/browser_QA false。证据：[窄继续march与回退验证](../reports/sc-resolve-2026-10-03/ddgi-zero-continue/README.md)。
- 下一步先区分status3非零无支持停止与新点仍非surface的语义，再连同次actual source/writer→SH/Gather/Fusion，不扩拒绝域/硬压history/nearest当首面；529/505/406失败不能无新证据重复。真实surface/N/rho、mesh支持域、native opacity和完整更早祖先仍缺，目标未完成。

## 540只读追加：native全零停步的原hit-band几何支持

- 520实际native last sample/八角值与540 actual list/ray/source4/query18全部16候选逐位连接到已核对的全原生装配几何；使用实际marchPos而非frac/终态tHi/CPU重建位置，stamp exact，CPU位置只按既有20µm重建界。native descriptor/texture证书保持520历史范围，不称本次新读取。
- analytic全零cross11184中last sample距全部几何中位1.935923m，284在原.45m band内、10897超原带+2mm；旧SC cross2779也有1476超原带+2mm，故不能全局改变主TraceSDF/旧选择。12点独立全262267三角穷举max3.551e−15m，其余仅全BVH重放/自洽；不把allzero/无50m首面或这些数量当sky/删history/灰亮root证据。
- 下一窄候选先完成原SC/screen，成功旧路径保持，仅失败analytic消费额外继续原march结果；对全零scalar/8角signed-cross先算原tHi再检查原hit band几何支持，未知保留原结果、无支持继续原step/miss分类，不用nearest距离直接替代步长/命中、不重推529 Finalize首面Lo/t/N替换。actual point/t/N与history→Gather/Fusion同次闭合，同binary/SPV开关、固定EV/全ray/原门，失败回退。当前last sample不是最终bounce点，不能把本轮资格直接推广到tHi。
- 本轮没有GPU/renderer/HDR/视觉改动，515244 SHA保持；新回归NOT_RUN_READ_ONLY，540新PASS历史另列、旧ratioFAIL/P0缺失保持。540capture38段保留37旧段；13raw新目录全部raw/JSON逐位重放，26自有副本SHA清理/statvfs增量.313GB，非独占回收。证据：[实际零停步几何支持](../reports/sc-resolve-2026-10-03/ddgi-zero-stop-native-geometry/README.md)。

## 540最新：原生几何装配全核对，临时C++回读已恢复515

- 当前实际GPU positions/elements/meshlet/u8 triangle/meshlet vertex/cluster map完整回读：161407顶点、2278 meshlet、262267带owner/朝向三角multiset=历史BVH mapping，missing/extra0，a/e1/e2逐位、cluster material=instance、world identity。element是属性不是索引；实际VisibilityBuffer.vert byte-packed u8，不能用PipelineDraw.vert u32旧路径解释。身份范围是completed host live-buffer数值/装配，仍非独立driver descriptor、visible compact列表或物理opacity证书。
- 新actual list/source4/query18全部16候选/ray/previousSH/relocated origins=536逐位，producer仅220 fresh [48].w padding7 volume echo不同、其他字exact。可对同查询回验旧actual slots几何，不把历史native fetch改称本次读取证书。CPU64/20µm actual rays解析18538中8911有50m首面/9627无，query到first中位3.722m，仅1≤2mm。全几何nearest中位1.863m，仅14≤2mm、9 flags0正面；12点独立全262267三角穷举max3.551e−15m，其余不称全穷举/物理root。
- 临时单C++源恢复、231SPV无变化/515244 SHA exact；fullserial DebugVulkan/Unit7+Debug5/新V350480/Metal≥65/原恢复SCstage PASS，三V native Gather全帧六ROI1%/Fusion2halfULP PASS，两350四SC/六main逐位/GPU112/13/80 PASS。诊断只350无480配对。旧ratio原非JSON门三V FAIL（--json exit0仅测量）；P0缺原baseline null NOT_RUN/历史FAIL分列。相机原128B ref SHA先核、inverseVP/camera对应20words逐位，不把选出80B hash比完整128B hash。
- 固定EV1.8546新图无GI收益：诊断→恢复whole-run真实indirect HDR右+.134787%、墙+.045489%、亮斑+.127597%，非隔离物理反事实。右墙/拱顶灰亮和层次差距仍在。网页540capture/37段保留旧536/36，comparison已查看/browser_QA false；163raw107unique、18同次raw新目录重放first/nearest/装配/JSON逐位。225自有副本核SHA清理，statvfs增量2.112GB，非独占回收。
- 下一步将actual全零zero-cross样本field来源/支持域、最近几何与actual first连接，再验证仅无surface资格停步继续原追踪的窄开关；不重复529 Finalize首面SC Lo/t/N替换、不按交叉份额裁history。旧SC/card/cacheT/16候选/射线调度、曝光/门保持，失败回退；物理opacity、真正query surface、完整旧history祖先和Gather/Fusion因果仍缺。证据：[当前原生装配与查询资格](../reports/sc-resolve-2026-10-03/native-ddgi-geometry-buffers/README.md)。

## 536只读追加：native history 连线 owner／材质资格

- 使用536实际37828 native slot/weight/query与actual relocated起点，历史529 CPU64/20µm首面11334交叉，159透射、976背面、9距query≤2mm；flags0/正面/两端>2mm 10190，仅几何资格候选数量，不是物理遮挡或屏幕/Fusion份额。名义起点14202交叉，3082slot判定变化；原CPU少1slot已由536阈值证据解释。
- 全262267历史triangle logical mapping→owner和低31位flags=536 resident GPU实例/material表逐项，393实例world identity、三资产当前SHA一致。源码raster/CardCapture不使用texture alpha discard、双面raster会翻N；flags0仍不是当前native positions/elements descriptor/texture/物理opacity证书，query表面owner/barycentric及完整旧history祖先未闭合。不凭这些交叉裁历史或重推已discard的修复。
- 无新GPU/HDR/renderer修改或视觉收益，515244 SHA在分析和清理后exact；新Unit/Vulkan/Metal/SCstage NOT_RUN_READ_ONLY，536历史PASS/P0缺失/旧ratioFAIL分列。网页536capture保留35旧段加第36段；独立目录原输入SHA、两组query/first/slot及JSON逐位重放，已归档本轮raw并核SHA清理。固定EV恢复图右墙/拱顶灰亮和层次差距仍在。
- 下一步使用GetGlobalVertexBuffer/GetGlobalElementBuffer窄范围同次回读，把当前native几何与mapping/world/material/actual slot连线first对应；继续query表面资格、writer更早祖先及Gather/Fusion贡献。原SC/card/cacheT/射线/曝光/门保持，合格候选再同binary/SPV开关验证。证据：[native history连线资格](../reports/sc-resolve-2026-10-03/ddgi-native-history-support/README.md)。

## 536最新：actual previous tetra native读取闭合，临时两源／231SPV全恢复515

- 同binary/231SPV的OFF/ON350、固定EV1.8546/ToneAdapt0/raySample0/zero-mode2、各≥65秒；27N原缓冲、query18vec/全部16候选保持，只有padding7开关切换临时44vec/ray写入空闲尾部，容量不足直接不写。原Lo/t/N/rho/SH/history/confidence/alpha、射线/调度/光曝光及旧选择不改。初次OFF query0使9N、原producer要求27N而REJECTED，无有效截图，不算收益；失败metadata/log留档。
- 新ON28160实际ray/18538analytic，9486grid内/9052外，37828实际slot读取、116权重阈值跳过；native probeID/权重/加载9vec4 SH/N/gp/result与本次position/stamp/ray对应。全部加载SH=同次before-history逐位，882×9 before-history=latest actual/preupdate source逐位；actual GPU IDs/weights重放原2e−5+5e−5 abs(expected)全PASS/max7.629395e−6，native result→actual rho history max0。独立driver descriptor地址、取光点表面/物理opacity及完整旧祖先仍未证。
- 531 ray8842残差已解释：CPU gp.y4.000999928→weight.000999928<.001跳过；native gp.y4.001000404→weight.001000404保留probe366。actual slots253/254/352/366，使用native权重重放value与GPU逐位，rho history(.417824388,.354802370,.265629053)。未放宽门；它纠正CPU/native跨既有阈值的重放差异，不宣称暗部/root或物理不连续。旧37827slot/RESIDUAL1记录作为历史保留。
- OFF/ON实际update-list/source4/query18/真实ray/previousSH/publishedSH/current Gather flat逐位，volume仅padding7不同；五项writer/Gather字节也核SHA实测=历史531。Fusion全帧字节不同，actual HDR右−.0000904%、左−.004094%、墙+.030350%、亮斑−.006191%、地−.001352%，两whole-run非隔离物理反事实，固定EV仍灰亮、质感与空间层次未达Lumen，无新视觉收益/性能保留。
- 临时两源+231SPV共233 originals全SHA恢复，生产515 retained244 exact；五份变化诊断模块私有目录重建=当次freeze SHA并spirv-val通过。恢复fullserial DebugVulkan/Unit7+Debug5/新V350480/Metal≥65/原SCstage PASS，四有效新V native Gather全帧六ROI1%/Fusion2halfULP PASS；两诊断350四SC/六main/GPU112/13/80 PASS。诊断未跑480配对，不能称该组SCstage350480 PASS。旧ratio四V FAIL，P0缺原baseline null NOT_RUN/历史FAIL分列。
- 536网页保留531capture/34段，新增5有效capture+1段为35；456raw/327unique SHA/gzip，10关键raw新目录native分析JSON逐位重放。488自有临时副本核SHA清理，logical unlink 2,776,462,824B、statvfs available增量2,780,241,920B（非独占物理回收），清理后244 SHA exact；保留capture/日志/源码/脚本，不删其他session、生产资产或构建对象。
- 下一步使用actual native slots/权重与实际relocated源点，补当前first/owner/material/alpha连接支持和取光点归属，沿实际writer更早祖先闭合SPGI/Gather/Fusion。不得把历史任意triangle交叉当物理遮挡或按份额裁历史，不重推已discard的normal/ρ/Eπ/首面替换。证据：[536 native previous tetra读取与恢复](../reports/sc-resolve-2026-10-03/native-ddgi-history-tetra/README.md)。

## 531只读追加：actual analytic previous tetra/history 支持

- 本轮无新GPU截图/HDR或渲染修改，515生产244 SHA exact；新Unit/Vulkan/Metal/SCstage NOT_RUN。current350实际18538analytic中9486grid内、9052外return0、37827有效tetra slot；CPU原.001权重/SH9/−RD/rho.5重放18537原容差PASS，一条实际ray8842 RESIDUAL，max.000170529，不放宽门、不称完整数学或native fetch证书。初次全inside假设/粗投影常量已纠正，残差未归因GPU/FMA。
- before-history882×9vec4全部等于latest actual writer（fresh350用本次preupdate source）；37827 slot前writer273..349，仅最新记录对应，不闭合完整旧祖先。源码tetra用名义网格、无visibility moments/relocated源点；当前位置偏移中位0/p90约1.2m/max2.4m。
- SHA恢复历史529世界BVH任意triangle连线：名义probe14201交叉、实际relocated源11333，3082slot判定不同。signed raw crossing historyY3343.128/2711.888、总15177.810，非物理遮挡/root/postclamp/Fusion份额/裁光因果。alpha/material/currentGPU几何/真实native slot/取光点表面仍待证，禁止据此直接裁历史或调曝光/射线/旧选择。
- 新目录独立重放analysis JSON/两组query-first/slot记录逐位，残差保持；网页仍531capture、保留33段加只读段为34。下一步补actual analytic previous native slot/weight/fetched SH witness与ray8842；几何可见性使用实际relocated源点并补owner/material/opacity。证据：[当前history tetra支持](../reports/sc-resolve-2026-10-03/ddgi-history-tetra-support/README.md)。

## 531只读追加：SC取光点与首面法线支持

- 本轮无新capture/HDR、无渲染修改；531 cards350与529 OFF350实际update-list/source4vec含RO/RD/stamp/query18vec逐位相同，三份场景/SDF资产SHA当前一致。历史BVH SHA恢复后新CPU64首面28160/17873 hit/0 unknown，两次输出逐位；与历史GPU hitmask一致但max t33.962µm，非全体20µm/当前GPU geometry descriptor/物理opacity证书。
- 8539原SC有8166首面/373无50m首面，8126 query早于首面20µm、40晚；query/first空间距离中位.788800m。1529 captured N·RD≥0中1472有首面、1469 query早/3晚、57无首面，空间间距中位1.395300m；captured/first几何N点积中位.381250（260反半球）。N可能来自不同表面且是mapped N，不支持全局翻转/删旧接受项/暗部根因宣称。
- shader旧SC路径直接读Lo，−RD主要影响SC失败后的analytic；此SC方向现象不直接证明writer SH错误。下一步继续取光点/first/owner支持及实际history祖先闭合，保留旧选择、不重复529失败替换。新Unit/Vulkan/Metal/SCstage NOT_RUN只读，截图/HDR仍531、无新增视觉改善，515生产244 SHA保持。
- 证据：[531实际SC取光点与首面支持](../reports/sc-resolve-2026-10-03/ddgi-sc-normal-first-support/README.md)。网页仍531capture，保留32旧段并加只读段为33；输入归档核SHA后仅清理本轮自有数据。

## 531最新：当前 SC texel、材质与 writer→Gather 补证；渲染保持515

- 两次新 Vulkan350、EV1.8546/ToneAdapt0/raySample0/zero-mode2、各≥65秒；第二次额外读回 resident card。生产13源+231SPV共244 SHA 清理后仍 exact，无 Engine/shader 修改。两次均原四SC/六main冻结与GPU112/13/80、native Gather全帧/六ROI1%和Fusion2halfULP PASS。新 Unit/V480/Metal/SCstage350480 NOT_RUN（本轮只读）；529恢复后的完整回归 PASS 仅作历史，旧ratio仍FAIL/P0原baseline缺失null NOT_RUN。
- cards350实际28160ray/220probe/8539原SC：resident64B card buffer=CPU allocation=main cards逐位；按本次selectedCard/query位置和resident card做CPU f32 UV/clamp/floor，8539实际RGB全部等于本次completed RGBA16F lighting atlas texel，maxerr0。它是数值与resident映射证据，尚非native每fetch地址/独立driver descriptor/raster拓扑/物理opacity/真实首面证书，不改变旧选择。DDGI读取total outgoing Lo（含direct），nodirect差值受half/clamp影响，不能叫精确direct。
- 选中texel LoY均值.760393、nodirectY .473556，rho均值RGB(.330319,.289263,.214784)；8539均valid128/无透射/无emissive，679合法black Lo保留。1529 captured N·actual RD≥0；query与depth重建captured点距离中位.585256m、最大1.278705m。原footprint/tolerance允许此偏移，这些现象未证明错首面/错法线/漏光根因，不能按计数删除原SC。analytic18538实际rho全.5，未重推已discard的E/pi或joint修复。
- current350实际882×9vec4 after=当前near flat SH=latest writer逐位，220 fresh同样对应；原epoch/source/projection/history/clamp公式门PASS。两次独立run的before-list/volume/hn/rays/actual-sh及ddgi350-irradiance六字段实测逐位相同，范围仅这六项。右暗廊signed preclamp Y：freshSC .00012356、analytic .00308708、negative .00020602、fresh history .16466573、nonfresh未知 .22479677；非postclamp/Fusion能量份额、非移除因果，旧history完整祖先未闭合。
- 对历史529恢复350的真实Fusion HDR右−.203%、左−.115%、亮斑−.046%；不同whole-run反馈，非隔离反事实。固定EV截图仍灰亮，颜色质感空间层次没有新增视觉收益，目标未完成；不调整曝光、光照、原门、射线或调度。
- 网页531 capture/32段，保留31旧段；146raw/66unique SHA+gzip。13关键raw在新目录恢复核SHA，payload JSON/逐texel记录重放逐位一致。182自有临时副本逐项核SHA清理；logical unlink 2,001,031,832B，statvfs available实测增量2,002,006,016B（非独占物理回收）。保留capture、日志、文本和重放脚本，不删除其他session、生产资源或构建对象。
- 下一步优先闭合1529实际mapped N方向和query/captured点支持与真实first/owner/material的关系，再沿邻像素writer/HN→SH→Gather/Fusion追旧history祖先；保留原SC接受项，不凭偏移或计数宣称根因，不重复529失败修复。
- 证据入口：[531当前SC texel身份与材质](../reports/sc-resolve-2026-10-03/native-ddgi-sc-texel-payload/README.md)，raw由该目录raw-manifest.json与scripts/restore_sc_stage_raw.py恢复。

## 529最新：原失败/allzero-cross首面SC窄候选DISCARD，完整恢复515

- 同binary/231SPV、EV1.8546/ToneAdapt0/raySample0/zero-mode2 OFF/ON350480各≥65；原SC/screen都失败且近场实际八fetch全零cross/无有效N，才尝试live首面owner/material opaque flags0/frontface/20µm重建与SC native validbit/非透射/2mm plane+bary支持。用已有Lo并使输出t/N同面；不改旧query/key/cacheT、射线/调度公式/SH/history/confidence/灯光曝光/原门，不重推rho/mappedN joint或4×4数组。
- 初次readonly oracle因固定409/410/507不在实际列表被拒绝，临时移除固定probe限制（完整list/容量/stamp/五生产缓冲before-after检查保持）；初次资格标记写在CB上传之后，GPU0，未启用，仅REJECTED记录。修正上传顺序后重建四capture；两项临时诊断修改也全回退，不能用失败组评估收益。
- OFF350实际28160ray/13963原scope，analytic11184，3653首面在停步后、1454独立GPU accepted。ON1312新回取，当前CPU64 t最大2.476µm/owner对应/N dot≥.999999924，但21独立GPU残差RESIDUAL6，native chosenSC texel/topology-material/物理opacity/history完整祖先仍未证。全28160 GPU/CPU hitmask exact，但max t28.116µm，不宣称全体20µm证书。原SH producer门PASS。
- 两组各220更新但仅174共同probe，观察到输出距离变化后的优先级列表不同（源码distance→depth variance→priority链，未闭合逐帧调度因果）；仅共同RO/RD/完整18vec含16候选/min-key逐位，各自身list/source/stamp闭合。ON7933 oldSC无retry重叠，主SC8190/correct3256/20旧缺口/350六main逐位冻结。不得宣称全OFF/ON旧DDGI ray/update-list完全对应或凭计数证视觉根因。
- actual Fusion indirect350右−19.907/左−6.527/亮斑−28.340/墙−16.842/地−13.064%；480右−20.751/左−8.806/亮斑−36.371/墙−18.528/地−14.623%。阴影收紧仍未达Lumen颜色质感空间层次；whole-run反馈非单帧反事实/能量份额。原SCstage direct_patch error+8.834/+7.948>2 FAIL，DISCARD，未改门。
- 六源+231SPV237 originals原字节恢复/helper删除，515 retained244 SHA exact/dirty保持；完整serial DebugVulkan/Unit7+Debug5/新恢复V350480/Metal≥65/恢复原SCstage与native Gather全帧六ROI1%/Fusion2halfULP PASS。候选四V+恢复二V的native原门均PASS，旧ratio六V FAIL/P0缺原baseline null NOT_RUN历史FAIL另列。无新GI/性能保留，目标未完成。
- 529网页保留520capture/30旧段，新增9capture含2上传失败记录；719raw/386unique SHA/gzip。43关键raw新目录重放native分析JSON、两组CPU64 query/first逐字节；775临时副本核SHA清理，statvfs可用空间增量实测5.115GB（同期活动可能影响），日志/截图/源与重建命令保留。[窄候选、失败与回退](../reports/sc-resolve-2026-10-03/ddgi-zero-cross-cache/README.md)。BOTTOM_OF_PIPE仅命令标记间隔。
- 下一步优先恢复515当前暗部/亮斑actual writer→Gather taps的原analytic sky/history与SC lighting Lo方向/单位/材质/同面支持及分量贡献，区分Lo与distance→depth/priority→history影响；补独立重建UNKNOWN与实际chosenSC texel读取。先只读同帧producer重放，不能固定旧update-list刷画面、放宽UNKNOWN/原门或再直接推广本候选；保持旧accepted/曝光光照/SH/confidence/完整射线，不凭1312或局部遮挡/UE流程宣称根因。

## 520最新：稳定当前近场 native SDF 模块/绑定/descriptor0/dispatch 与实际角点读取闭合

- 只留证据，七源+231SPV238 originals全部原SHA恢复；515 DUMP2帧末修复保持，retained515244 exact。恢复touch cpp/h/inl确保native helper依赖翻译单元重编。完整serial DebugVulkan/Unit7+Debug5/新V350480/Metal≥65/原SCstage/native Gather全帧六ROI1%/Fusion2halfULP/四SC六main/GPU112/13/80 PASS；旧ratio四V FAIL/P0缺原baseline null NOT_RUN历史FAIL另列，目标未完成。
- native独立prefix exact DUMP2+IDENTITY1接已有记录器，无analytic prefix/额外setter；临时48N actual source/query/stop/fetch与explicit schema。首构建备用输出名first_surface/first_native_normal错误触发原firstsurface/firstnative freshness拒绝，未注册两文件核SHA另存移除，原宏/正确名修复/231spirv-val及完整构建PASS，失败保留。
- 同binary OFF/native350原SC六binary逐位、旧8190/correct3256/20恢复/card-cacheT保持。4真实TraceSDF创建serial1/2无bind/dispatch，serial3近场/serial4另一volume各353bind与353dispatch；实际模块SHA、module/create/bound pipeline-layout、native sets实际Vulkan descriptor0写入对应同次SDF handle/native view，HN binding6/list binding5对应actual近场源。另一volume独立未认证，缺per-frame CB write sequence tag，不称任意driver descriptor内存/全volume/物理证书。
- 近场220probe/28160ray/28155场内，225240原sampleSDFCascade d000..d111实际texelFetch操作数/IDs（非另读或CPU重构）=同次完成384³ R16F回读逐字节；回读=冻结cache payload逐字节。actual stamps/probe/ray/step与原16min-key PASS。当前实际RO/RD新20µm..resident50m CPU64 first重算17873hit/unknown0；1159旧GPU fixture自检1008hit/151miss/最大2.616µm原20µm PASS，仍非当前GPU first/LOD/变形/opacity/sky或暗部因果证书。
- 13963 signed-cross实际8角全0：6625有冻结首面、6170早/455晚，7338无50m首面。actual before-ray+HN原query/witness分类8539旧SC（2779cross/2517早停全零cross），18538analytic（11184cross/3653早停全零cross），1083negative；analytic albedo取shader定义vec0.xyz全(.5,.5,.5)，不误读unused vec3，也不以private writer清空negative行判positive/source。private writer完整epoch/SH/history本轮没有新证书。
- 新native350比恢复350真实间接HDR R−.1345/L−.1018/墙−.1363/地−.0601%，whole-run反馈非隔离收益；固定EV仍灰亮、质感层次未达Lumen，不推广GI/性能。520网页新增5capture/保留29旧段，441raw/320unique SHA/gzip；18关键raw新目录CPU1159+28160重算逐位、native identity JSON仅归一化输出根路径后exact、stop-first JSON exact。470临时副本核SHA清理，statvfs可用空间增量实测约2.52GB（同期活动可能影响），[520原生读取证据](../reports/sc-resolve-2026-10-03/native-ddgi-sdf-bound/README.md)。BOTTOM_OF_PIPE只称命令标记间隔。
- 下一步先对原SC/screen都失败且零cross的analytic实际范围（3653当前早停候选），补同次GPU first/material/SC texel支持与coherent point/t/N，再做窄资格同binary/SPV OFF/ON350480视觉/真实HDR与原门对照。保持全部old accepted，不能把无50m first/allzero改sky，不能拿数量/UE流程/局部遮挡当根因；不重推discard宽正距离/joint/4×4数组、不改光曝光/SH/confidence/射线/旧选择/原门。目标未完成。


## 515最新：DUMP2导出提前return导致重复外部帧已修复并KEEP

- 只改TestVulkanSponzaRenderGraph.cpp：DDGI_DUMP2导出后进入正常帧末captureTarget释放/外部frameCount递增与检查；else保持原SH-only导出互斥，默认无dump路径不变。renderer数学仍487/231SPV全原SHA，其余retained243 exact/全部dirty保留；新244状态见[retained244.json](../reports/sc-resolve-2026-10-03/ddgi-dump-frame-epilogue/experiment/retained244.json)，后续以此为原SHA基准，不能用旧487 TestVulkan SHA覆盖本次已保留修复。
- 511原SC帧偏移机制现已闭合：旧DUMP2 return跳过外部frame计数，内部pipeline已前进，下一迭代再次外部350且覆盖SC oracle/PPM。旧511 off350日志两个Frame350 capture，新一次；新DUMP350 8190 main samples及六binary全部等于原冻结，frame350:3841/351:4349，3256正确/20旧地面恢复/旧card/cacheT保持。不是修改oracle帧或门禁伪对齐，也不是同帧后续导出直接改第一次回读，更不证明SDF/灰亮根因。
- 新同binary control350/DUMP2-source-query-producer350/control480/Metal≥65，固定EV1.8546/ToneAdapt0/raySample0/zero2/完整射线。fullserial DebugVulkan/Unit7+Debug5/原SCstage/native Gather全帧六ROI1%/Fusion2halfULP/四SC六main/GPU112/13/80 PASS；实际source meta、producer/resident CB frame350与更新probe IDs对齐。旧ratio三V FAIL/P0原baseline缺失null NOT_RUN历史FAIL分列。driver binding证书仍未跑，不推广GI/性能。
- 新DUMP350比control350真实间接HDR R−.0001/L+.0192/墙+.0054/地−.0168%，whole-run反馈差异非隔离收益，固定EV仍灰亮、颜色质感/层次未达Lumen。515网页新增4capture/保留28旧解释段，133raw/71unique SHA/gzip；34关键raw独立新目录重放Gather/Fusion数值JSON exact（仅归一化输出根路径）、SC main JSON/六binary逐位；215临时副本核SHA清理，statvfs两段可用空间增量实测合计约2.56GB，见fresh-replay.json/cleanup.json。[515控制流证据/回归](../reports/sc-resolve-2026-10-03/ddgi-dump-frame-epilogue/README.md)。BOTTOM_OF_PIPE只称命令标记间隔；临时raw核SHA清理后按本阶段manifest恢复。
- 下一步使用修复后的帧生命周期，重采48N停步/实际角点与独立native日志prefix（现有helper不接DDGI_NATIVE_SDF_IDENTITY_PREFIX，不能借analytic prefix触发额外setter后拼范围）。先闭合实际首面/point-t-N/材质与SC texel支持，再做原失败分支窄资格同binary对照。无50m首面/allzero不等于sky证书；不重推discard joint/宽正距离/4×4数组，不改光曝光/SH/confidence/射线/旧选择/门禁。目标未完成。


## 511最新：当前粗场实际停步/texel补证，SC帧等价失败诊断已回退

- 生产仍487，临时48N诊断六源+231SPV237 originals全部SHA恢复，累计retained244 exact/既有dirty保留。完整serial DebugVulkan、Unit7+Debug5、新V350480/Metal≥65、原SCstage/native Gather全帧六ROI1%/Fusion2halfULP/四SC六main/GPU112/13/80 PASS；旧ratio四V FAIL/P0缺原baseline null NOT_RUN历史FAIL分列，无视觉/性能推广。
- 核SHA恢复506 actual producer350同28160 RO/RD作新float64 BVH查询：SC8539有首面8166/无373（8126早40晚），analytic18538有8911/无9627（8116早795晚），negative1083有796/无287。原1159 GPU首面自检mask逐位、最大t误差2.616µm/原20µm PASS/unknown0。仅冻结CPU几何，不是当前GPU首面/opacity/LOD/sky资格；.05/.45m是诊断桶，不能改门或把无50m首面变sky。
- 新native350 actual28160ray，28155场内/225240角读取实际texel IDs/值与同次384³ R16F完整回读逐字节，回读与冻结cache payload逐字节；原16min-key/stamps通过。逐位核对506与新actual probe IDs、RO/RD与stamps后，只连接冻结CPU first到新native stop t/texels：13963 signed-cross八角全0，其中6625有首面、6170早/455晚，7338无50m首面。Finalize t/radiance不逐位，不能跨run拼SH/source收益证书；driver create/bind/descriptor/dispatch日志缺失，物理/暗部根因未证。
- 诊断OFF/native samebinary350 samples彼此逐位，但原冻结SC main门 FAIL：基线frame350:3841/351:4349，新351:4349/352:3841；3841条direction改变、3779firstT改变。pose实际inverseVP/camera逐位，SC shader未动。帧偏移原因未证（full DDGI_DUMP350生命周期仅假设，full dump在main stats回读之后）；不得改oracle帧/门伪对齐或推广旧选择证书。初次strict27N输入48N InitReject、首次shader freshness拒绝均保留；之后显式bounded48schema与原宏两备用SPV修复后构建通过，最终全部回退。
- native日志缺失原因：新DDGI_NATIVE_SDF_IDENTITY_PREFIX未接既有ddgiRecordedComputeAuditPrefix；helper仅DDGI_FIRST_ZERO_CROSS_AUDIT_PREFIX在原exact开关下或DDGI_ANALYTIC_FIRST_RHO_PREFIX。标量byte证据不是driver绑定证书。新native350比恢复350真实间接HDR R−.5330/L−.3105/墙−.3093/地−.1987%，不同SC帧/whole-run反馈，无隔离收益，固定EV仍灰亮/质感层次未达Lumen。
- 511网页新增5capture/保留27旧解释段；456raw/332unique SHA/gzip，15关键raw新目录CPU1159+28160重算逐位，geometry/stop-first JSON逐位、scalar除路径keys外exact且SHA values exact。[511证据/失败/回退](../reports/sc-resolve-2026-10-03/native-ddgi-current-sdf/README.md)。489临时raw副本核SHA清理，文件分配空间约2.79GB；按manifest恢复；不引用消失路径当新证据。BOTTOM_OF_PIPE只称命令标记间隔。
- 下一步先闭合诊断真实帧序列/资源范围与正确native prefix，保持SC冻结记录；再在同RO/RD cohort补GPU首面、原停步取光点/距离/N/材质和SC texel支持，并连接同帧writer→Gather/Fusion。禁止将无首面/allzero直接改sky、宽正距离/已discard joint/4×4数组、alpha/SH/confidence/射线/旧选择/曝光/灯光/门禁改动。目标未完成。


## 506最新：actual current350 writer→native当前Gather闭合，原N回退来源有同帧证据

- 本轮只读，不改Engine/Shader/231SPV或渲染实现；生产仍487、retained244 SHA exact。先核SHA恢复505候选同次32raw：Gather350的882近SH仅663与writer349 actualAfter逐位，fresh349仅41/220一致，右廊fresh349 consumed槽0。此范围检查纠正“349新ray可覆盖350右廊”的错误前提，不拿历史候选替代本轮baseline。
- 既有DUMP2/source/query/producer350新baseline350 capture，固定EV1.8546/ToneAdapt0/raySample0/zero-mode2/≥65；actual binary/231SPV/assets freeze与505恢复相同，ref12° inverseVP+camera逐位。actualAfter350882×9vec4=同帧native Gather当前near flat逐位（flat2930/near offset0），882 latest writer记录各自epoch与actualAfter逐位，220fresh全匹配。原epoch/source direction/投影history/clamp门限2e−5+5e−5|expected|、private/actual容差通过，private/actual不逐位；完整backend driver descriptor/物理opacity/完整history祖先证书未跑。
- 220actual更新/28160ray：8539原SC、18538analytic sky/history、1083negative、positive未分类0。analytic native rho全(.5,.5,.5)，source10/11/12/14=2163/7267/446/8662。HN按update-slot且原list映射/frame-probe-ray stamp exact；coarse/depth evidence mode0、material projected有效0。27077正距离的原绑定HN细场gradient valid0，与同ray原Finalize RD+shader唯一分支闭合N=−RD；不是实际triangle N或独立driver fetch证书，negative不执行hit N，不能将其推导值当实际使用。
- 原fresh source cohorts→actual SH投影/alpha.02/history/band clamp→native当前ID/归一化regular-low权重/N/confidence作signed preclamp线性分解，原容差闭合。右廊Y均值freshSC .00012356/analytic .00308708/negative .00020602/fresh history .16466573/非fresh来源未知 .22479677。**不是post-clamp/Fusion能量份额或删source因果**，旧history祖先/非fresh实际query来源未知；不据此改alpha、SH、confidence、删history/压暗或整批换N。
- 新native全帧六ROI原1%与Fusion2halfULP、四SC/六main exact/GPU112/13/80PASS，原main8190/correct3256/20保持。只有新350，SCstage350480对门/Unit/Metal本轮NOT_RUN（无Engine变化），505历史构建/回归PASS单列，不混用；新350旧ratioFAIL、P0缺原baseline null/NOT_RUN历史FAIL另列。相对505恢复同binary历史350实际Fusion HDR右−.13458%/左−.11805%/墙−.12829%/地−.06174%，whole-run非单帧隔离/全图等价；当前DDGI Fusion份额右.527835/左.598991，与上述preclamp分解不同量。固定EV仍灰亮、颜色质感层次无新增收益，不推广视觉/性能，目标未完成。
- 506网页新增1capture，保留26旧解释段，83raw/57unique SHA；32关键raw独立新目录三分析JSON byte exact；115临时副本核SHA清理实释1090842624 bytes（约1.09GB），capture/log/JSON/scripts保留，raw按506manifest恢复。[同帧writer与法线来源证据](../reports/sc-resolve-2026-10-03/native-ddgi-current-writer-gather/README.md)。下一步在这些actual当前ray范围补native SDF stop/sample粗场读取与实际首面/材质/SC lighting payload支持身份，核取光点/距离/N同面；不拿N=−RD现行回退或505更暗就宣称根因，不重推失败joint/宽正距离/4×4数组或改光曝光/原门。BOTTOM_OF_PIPE只称命令标记间隔。



## 505最新：DDGI失败查找的实际首面SC回取暗部改善，原亮部门禁FAIL，DISCARD

- 同binary/231SPV开关PRIMAL_DDGI_FIRST_SC_RETRY0/1、固定EV1.8546/ToneAdapt0/raySample0/zero-mode2的350与480对照。只在原SC与prev-screen均失败后，用真实射线两层BVH20µm/64stack首面、同owner目录、CPU实例/材质及GPUflags0、正面、native capture validbit/非透射/2mm plane+bary回取已有SC radiance；旧accepted SC/screen、原SDF距离/法线、完整128ray/更新、SH/history/confidence/曝光光材质保持，不放宽旧query/key/cacheT。不使用已discard joint point/rho/mappedN 或4×4数组。
- 真实Fusion间接HDR右廊−45.6988%/−48.5590%、左−23.1645%/−27.2036%、墙−13.5758%/−15.7940%、近地−5.1982%/−6.1986%。前景拱顶/柱后墙亮雾减弱、石材仍见；相对Lumen仍灰亮，颜色与整体质感未匹配。whole-feedback-run非单帧隔离因果，不能仅按暗部下降就推广。实际view+camera ref12°OFF/ON/原生/恢复全部逐位。
- **原SCstage两帧FAIL，DISCARD**：direct_patch参考误差增量+10.733/+9.782超过原非目标≤2；其它五ROI通过，暗部收益不能抵消亮部退化，不改门。native DDGI全帧/六ROI原1%及Fusion2halfULP全部通过，不能替代视觉门；旧绝对ratio四V组FAIL，P0原baseline缺失null/NOT_RUN历史FAIL分列。直接光/材质常量未改，raw并非全图逐位：350 direct仅26像素不同/最大.000124，Gbuffer逐位；480两像素不同，不能称同帧全图等价。
- 独立native350 DUMP2/source/query audit与producer349，PATH_DEBUG不启用：220实际更新probe/28160ray，HN按update-slot、原before-list映射，原frame/probe/ray stamps exact。8910原SC winner、4357实际firstSC retry、13760analytic sky/history、1133negative，positive未分类0；retry原SC仍miss、原analytic witness0，实际输出与原epoch/投影history门限2e−5+5e−5|expected|通过，private/actual非逐位。不是SH/screen份额或完整history/driver descriptor/cache native texel payload/物理opacity证书。
- 4357retry全部原SDF正距离，无t0；4015首面更远、342更近，无2mm内一致，中位原t.656m/首面4.158m。候选保留旧距离/法线，取光点与距离/法线同面尚未闭合；不是亮部失败根因证明，不据此调weight或再次推广整批首面resolve。
- 四原源+231SPV235 originals恢复、新helper删除，487 retained244 SHA exact/既有dirty保留。强制DDGI header及调用方真实fullserialDebugVulkan重编，Unit7+Debug5、新V350480/Metal≥65、原SCstage/fourSC/sixmain/GPU112/13/80及native DDGI/Fusion门禁PASS；原SC8190/correct3256/20地面恢复保持。只构建DebugVulkan，没有交叉污染共享libEngine.a。
- 505网页新增8固定EV截图，保留25旧解释段；489raw/327unique SHA/gzip，17关键raw新目录重放native分析JSON byte exact；506临时副本核SHA清理实释3172642816 bytes（约3.17GB），截图/日志/脚本保留，raw按505manifest恢复。不保留新渲染/性能改动，生产仍487（448+机位锁），目标未完成。BOTTOM_OF_PIPE只称命令标记间隔。[505证据与回退](../reports/sc-resolve-2026-10-03/ddgi-first-surface-cache/README.md)。下一步闭合实际首面SC texel payload、亮部ray/SH→当前Gather/Fusion与点/距离/法线/插值支持，不能按命中数、局部阴影或UE流程宣称根因。



## 497最新：DDGI349实际writer→Finalize350 previous SH闭合；fresh analytic uniform albedo证据

- 同binary/SPV OFF/ON350固定EV1.8546/ToneAdapt0/raySample0/zero-mode2，双方既有DUMP2、PATH_DEBUG未启用；临时仅SPGI两源snapshot，原shader/231SPV/射线不改。21 SPGI snapshots frame350/read353。349 actualAfter882×9 SH = 350 Finalize previous binding11/host243 byte exact，host349 output243/slot1/220updates；882 latest实际writer记录与actualAfter逐位，各自保留epoch。原epoch/current CB-list输入echo/source RD stamps/投影history analytic门限2e−5+5e−5|expected|PASS，CPU绝对max.00024414不是1e−5声明；private/actual不逐位/native compare max3.81e−6。完整旧driver descriptor/native-certificate verifier与独立349 fresh-ray metadata未运行，不能升级成driver/物理证明。
- 核SHA492历史选择JSON，1141当前bound position八CPU possible corners=39 DDGI IDs（非native active tap/Gather lineage）；latest writer319..349，16在349更新，origins350与latest writer一致，原alpha全.02。4992ray正距离4633/exhaust359、zero与sky escape0，原adjusted ray Y和97.7441%正距离。仅不同latest epoch的源ray诊断和，非SH/屏幕energy，也不排除更早history天空；下一步优先正距离DDGI真实表面/取光资格。
- **fresh349 native子集**16probe/2048ray，HN按update-slot、producer记录按absolute-probe；初次直接absolute切HN被stamp REJECTED留档，改原before-list映射后frame/probe/rayID通过。776原SC winner、1216原analytic sky+history witness、56负距离，ray Y和65.8084%/29.4774%/4.7142%。1216 analytic原albedo全部(.5,.5,.5)，source10 behind338/11 outside504/14 coarse-vs-raster374，raw skyY82.39645/history238.76892（未含距权重）。不是灰亮根因或完整screen贡献；下一步在原positive analytic/SC真实射线上补首面、材质flags/UV/纹理/native读取证据，考虑只修失败查找的窄几何资格，保持旧accepted SC/screen、原weight/SH/confidence/曝光，不改uniform fallback常量刷颜色。
- OFF→ON whole feedback HDR R−.202674%、L−.115038%、wall−.136399%、floor−.065645%；native view+camera reference12°逐位，实际图片拱顶/墙面仍偏亮，无颜色/层次收益/性能推广。临时2源+231SPV233originals移除，487 retained244 exact，旧dirty保持；header/SPGI/Standard依赖真实fullserialDebugVulkan、Unit7+Debug5、新V350480/Metal≥65、SCstage/fourSC/sixmain/GPU112/13/80与native DDGI六ROI/Fusion原门限PASS。旧ratio四V组FAIL，P0缺原baseline=null/NOT_RUN历史FAIL另列。
- 38关键raw新目录三分析JSON byte exact；本轮4个tmp目录核SHA清理443副本、实释2185687040 bytes（约2.19GB），保留capture/log/script/JSON/source备份，raw按497 manifest恢复。
- 497网页保留24旧解释段+新段，405raw/319unique SHA/gzip跨阶段同SHA硬链接。来源/重放/清理见 [497归档](../reports/sc-resolve-2026-10-03/native-ddgi-previous-writer/README.md)。生产仍487（448修复+机位锁），目标未完成；BOTTOM_OF_PIPE仍只称命令标记间隔。


## 492最新：actual Finalize previous DDGI八tap→原ray radiance→Average SH，临时诊断移除

- 新31 GPU snapshots（frame350/read353），1141历史选择probe/54768ray仅作选择、非当前Gather lineage。30135实际world DDGI（25702投影）；16250 zero SDF负w全部native world DDGI，原binding11 previous SH 882probe/host handle243与八tap ID/权重/评估值按原1e−5闭合（SH max9.54e−7/插值4.77e−7）。Finalize原radiance及Finalize/Average actual RD+invNq byte exact，47172原64lane→Average SH RGB uint32 exact/max0。zero借用56.4012%仅选中probe preconfidence L0和，不是屏幕energy/物理漏光根因。
- Gather current flat binding17/host handle2960是不同资源；256B头后near volume5691 RGB值不同，max .05439758/mean .00035003，不能替代previous重放。原SC retry status3=first==0、4含法线/正面失败、6GPU材质flags、10同owner card失败；positive firstT不是成功首面证书，未改变旧选择/未跳过未知首面。
- 同binary/SPV OFF/ON350实际view+camera reference12° byte exact，EV1.8546/ToneAdapt0/raySample0/mode2。真实间接HDR R+.2059%、L+.0993%、wall+.1311%、floor+.0756%，whole feedback run非单帧隔离因果。实际图片拱顶/墙面仍偏亮，无颜色/层次视觉改善；不推广性能或诊断开关。
- 临时5源+231SPV236 originals全部恢复，487 retained244 SHA exact；header/SPGI/Standard强制真实依赖重编，完整serial DebugVulkan构建、Unit7+Debug5、新V350480/Metal≥65通过。新350480 native pose逐位，SCstage/fourSC/sixmain/GPU112/13/80及native DDGI六ROI/Fusion原门限PASS；旧ratio四V组FAIL，P0缺原baseline=null/NOT_RUN历史FAIL独立。原有dirty全部保留，生产仍487（448修复+机位锁一源），没有新增GI渲染改动。
- 本轮4个tmp目录已核SHA清理434 raw/bin/SPV副本，实测释放2477596672 bytes（约2.48GB），capture/log/JSON/script/source备份保留。关键raw从492 manifest恢复，不读已删除临时raw。
- 492网页保留23旧解释段+新段；raw402/unique309逐SHA gzip、跨阶段相同SHA硬链接。32关键raw新目录重放两份分析JSON byte exact。下一步从同run frame349 DDGI真实writer actualAfter到frame350 Finalize previous SH做闭合，再连接实际邻居输运资格/当前Gather屏幕贡献；driver descriptor/真实opacity/完整history根因仍未证，不按56%删回退、改权重或压暗。BOTTOM_OF_PIPE只叫命令标记间隔。目标未完成。[492证据与复现](../reports/sc-resolve-2026-10-03/native-spgi-finalize-world-ddgi/README.md)。


## 487后只读补证：world DDGI条件及previous/current读取身份

- 核SHA487实际输入，16243 zero SDF/B5负号ray的probe position全部在已启用DDGI体积；visibility0、spacing3/counts14×7×9，CPU原8邻点wsum .370761..854436、有效anchor距离min .405008m/39IDs，原SDF与Gather globalCB byte exact。仅满足原world DDGI前置条件，非原Finalize native branch/fetch证书；WRC源码只在far hit.w3，不能把world借用继续泛称WRC，也不把56.48%选中SH L0系数和当屏幕energy/根因。
- 源码SPGI Finalize binding11经GetVolumeViewBySlot(previousIndex)，native DDGI Gather binding17经GetVolumeView(frameCount).currentIndex flat copy+256B头；不同slot/buffer路径，**禁止用Gather current SH替代Finalize previous SH重放**。实际Finalize handle/内容/native branch/taps及previous writer仍UNKNOWN。下一步同capture补binding11 actual previous SH和原world分支/tap fetch，连接actual q/first/material/source与本帧native Gather；不改slot、visibility、归一化/confidence/光曝光或按计数删分支。
- 无新renderer改动/无新capture或视觉收益，当前retained244 SHA exact，分析独立重放byte exact，截图仍487；487回归仅历史结果、旧ratioFAIL/P0未验历史FAIL分列。目标未完成。[可重放条件与读取路径](../reports/sc-resolve-2026-10-03/spgi-world-ddgi-preconditions/README.md)。


## 2026-10-05 最新：actual importance校正权重→Average SH闭合，诊断移除／capture机位锁补缺（487阶段）

- 新28 actual GPU snapshots、frame350/read353 checked；核SHA历史458 raw19/receiver JSON，只选1141 probe、非当前Gather tap。初始历史raw被清理造成旧脚本空循环已REJECTED，补严格19数量并恢复核SHA，选择ID不变，不能把空循环当认证。原SDF/Avg invocation实际RD/invNq；Average原0..7保持新增8记录/9map，实际basis/clamped及weighted radiance。47167参与ray原64lane归约及4π/48重放按原1e−5 PASS，**actual SH RGB max0且uint32逐位**；46678 world有效ray SDF/Avg RD与校正权重byte exact，同capture importance buffer及原seed350/isMode1/tier0闭合。Spatial输出=Gather SH、Denoise输出=Fusion SPGI byte exact，不替代完整history祖先/本帧native Gather selection。
- 仅选probe、confidence/history前SH L0系数和：zero SDF正几何17235ray占20.3400%，负借用16243占56.4836%，其他有效13689占23.1764%。不是屏幕energy/物理visibility/opacity或灰亮根因；B5 sign不能分具体DDGI/WRC/sky/old-new SC，source texel/true first/当前Gather lineage仍UNKNOWN。invNq .231687..3.923594是原proposal校正权重，不从w/dot伪造q/拿uniform替代。不按份额删回退或调confidence/SH系数。
- 同binary/SPV OFF/ON350 EV1.8546/ToneAdapt0/≥65秒actual Fusion间接右−.537832%、左−.307551%、墙−.397611%、地−.209463%，whole-run反馈非单帧因果；实际native view均reference yaw12，不是仅凭静态metadata。固定EV仍灰亮，无明确颜色质感层次收益，诊断全移除/ON480未采/无性能推广。四source+231SPV235original exact；初次恢复448244SHA exact、强制header/SPGI/Standard全部依赖实际重编/fullserialDebugVulkan/Unit7+Debug5/Metal≥65。
- **初次恢复350 SCstage PASS/480 direct_patch+sunlit_wall FAIL原结果保留**。该480 actual native inverseVP/camera yaw≈2°而SponzaReference.h参考12°，静态capture metadata未反映；历史479 raw13/meta核SHA后实际view/albedo/ORM/direct也非同输入，不能当仅GI反馈差。旧恢复binary独立repeat480回12°，与原350配对原SCstage PASS；不覆盖原FAIL。源码SPGI_CAM_LOCK仅锁WASD/mouse Update，方向键旋转与R-reset未保护；**仅TestVulkanSponzaRenderGraph.cpp加相同presence guard**，保留交互模式/scene/light/exposure/material/ray/SH。原偏移键事件未native记录，完整事件因果NOT_PROVEN；这一代码锁定缺口与新锁定验证分别陈述。
- serial target实际重编后新locked350480 native inverseVP16float+camera4float与reference逐位、原baseline SHA SCstage/native DDGI全帧六ROI1%/Fusion2halfULP PASS；有效350原四SC/六main exact/GPU112/13/80/8190正确3256、20保持。保留448+**验证机位锁补缺一源**，其他243初始SHA exact/新retained244 SHA exact/235诊断original exact/此前dirty保持；不是GI视觉提升。旧ratio全部7组Vulkan FAIL；P0缺原baseline未验/null历史FAIL分列。BOTTOM_OF_PIPE仍仅命令标记间隔。
- 网页487保留21旧解释+8新captures，含原机位偏移FAIL、旧binary重复及补锁新350480；[权重/actual pose/真实HDR/恢复与raw SHA](../reports/sc-resolve-2026-10-03/native-spgi-q-average/README.md)。下轮baseline用本阶段experiment/retained244.json（原244仅一Test源变），先核SHA。下一步同capture actual Finalize old/new card、BVH first owner/material/primitive/source fallback/native texel，配actual Average校正权重及当前native Gather/Temporal历史贡献；不能拼474/448不同run作首面证书、凭静态metadata证明相机、调曝光/normalfloor/alpha/sigma/confidence/SH或重推4×4线段数组。目标未完成。


## 2026-10-05 只读补证：局部 SC capture 插值无新增资格，截图仍479

- 核SHA恢复448追加生产frontguard之前owner350原始16输入；main depth与bound SPGI depth byte exact、main card有效前缀exact、main atlas frame350。IDs来自同次main readback，不冒充Finalize原invocation fetch/driver descriptor；CPU以native first point/t和原biased probe origin重建RD，不拼当前生产/474 source或Gather证书。
- 原owner retry失败314729，CPU保守front280283；774421内部朝向ray/card pair同primitive角点0..4为714842/36021/17947/5535/76。严格same triangle/owner/有效非传输有限capture/各点plane≤2mm+baryinside/凸插值误差≤2mm：四角76 ray、同2×2三角simplex227pair/151ray；全部未通过原hard hint(faceGate0.25/hintSoft0)，四角最近texel原along/perp已通过。保持原查询条件时这批无新增合格支持，不加局部插值/放宽hint，不宣称hint或capture覆盖为灰亮根因，不重推4×4世界线段数组。
- renderer244 source/SPV SHA exact/此前dirty保持，分析独立重跑byte exact。无渲染改动、无新GPU capture/build/regression/视觉收益，479与既有原门结果保持历史身份、阶段数不增加；旧ratioFAIL/P0缺baseline本轮未验历史FAIL分列。下一步当前actual ray/q→first/material/source texel→Average/Gather/Fusion贡献；目标未完成。[可重放历史支持分析](../reports/sc-resolve-2026-10-03/sc-capture-support-history/README.md)。


最新479：Spatial opaque首面邻接资格DISCARD，保留448默认关mode2。临时readonly几何bindings4..9/原16B CB末pad enable+frame，只对原center→neighbor真实20µm/64stack首面identity/frontface/Opaque alpha1 graph0/actual flags0/owner-material-frame及两端距first plane均>原2mm异侧排除；未知首面不跳过、近面同面保留，原sigma/center/distance weight/SH-confidence公式和完整ray/history路径保持，Metal原路径。samebinary OFF/ON350真实HDR右+0.087506%、左+0.044188%、墙+0.061577%、地+0.005483%，固定EV没有明确颜色质感层次收益；回退，不称whole-run差异为单帧反事实/全图等价，逐link native资格NOT_PROVEN、不能排除全部空间混合根因或拼458历史证书，候选480未采/无性能推广。两源+231SPV233恢复/全部original byte exact/448244SHA exact/dirty保持/header未改，fullserial实际SPGI重编/Unit7+Debug5/新mode2V350480/Metal≥65/原SHA SCstage+native原门PASS，四SC/六main/GPU112/13/80/8190/3256与20保持，旧ratioFAIL/P0缺baseline未验/null历史FAIL。网页479及spgi-spatial-visibility归档5新capture/HDR/raw SHA。继续原始ray actual q、同capture BVH first/material/SC radiance支持及当前Gather/Fusion，不无新证据重推post sigma-alpha-confidence/history/该spatial候选；474已证本批t0为coarse first sample/detail active0，非精修，仍非root。目标未完成。

最新474：原始SPGI SDF invocation记录完整回退，保留448默认关mode2。核SHA历史458右左IDs只选1141probe，非本帧screen tap证明；新25actual GPU snapshots原54768ray/616HZB skip/54132SDFhit/20farcache，38921 finalt0全coarse首次样本且lastpos==origin，原detail active0/no refine，不能再把该批零距离归因精修；负/零/正native d6073/4322/28526。33466 outward actualFinalizeradiance B5几何17222/借用16244，5455非正w0不投影；非SH/能量/屏幕份额。SPGI F2 importance当前开，DDGI_RAY_SAMPLE0不关闭它，本轮未记录invNq，SH projection明确UNKNOWN/null，不从w/dot伪造q或拼旧433/448capture做同帧证明。下一步沿新actual原origin/direction/lastscalar补同capture BVH first/material/source texel与native q→Average/currentGather/Fusion，不按计数删回退/改threshold-bias-sigma-alpha-confidence-SH系数/曝光光或重推discard。记录OFF/ON samebinary350实际HDR右−0.536337%、左−0.315992%、墙−0.327082%、地−0.217596%，whole-run反馈非只读单帧因果，无明确颜色质感层次收益/无新生产性能修复，诊断480未采。三源+231SPV234恢复/448244SHA及全部234original exact/dirty保留，强制header/SPGI/Standard依赖实际重编/fullserialDebugVulkan/Unit7+Debug5/新mode2V350480/Metal≥65/原SHA SCstage+native原门PASS，四SC/六main/GPU112/13/80/3256与20保持；旧ratioFAIL/P0缺baseline未验/null历史FAIL。网页474及native-spgi-sdf-producer归档5新captures/真实HDR/原始SHA。目标未完成。

最新469：Temporal历史源反馈对照DISCARD，保留448默认关mode2。原Temporal读上帧Spatial之后Avg；临时0原路径/1原来源加一依赖/2同依赖下读Spatial前Temporal，完整本帧射线/原alpha-sigma-SH-confidence与最终Spatial/Gather保持，只有宿主上帧Temporal writer连续才选新源，非GPU ancestry证书。新同binary/SPV350 actual Fusion右+0.271517%、左+1.670613%、墙+2.023395%、地+0.529423%，控制1对0六ROI<0.1%，无明确颜色质感层次收益，来源及单依赖均回退，不称完全等价/递推拓扑或同步根因，候选480未跑。两源+231SPV恢复233/448244SHA exact/此前dirty保持。首回退header copy2旧mtime导致调用方候选类布局object残留，f1无效HZB/buffer/VUID原门拒绝留档；强制touch恢复header+SPGI+Standard后fullserial实际全部依赖重编/Unit7+Debug5/新mode2V350480/Metal≥65/SCstage+native DDGI全帧六ROI1%与Fusion2halfULP PASS，四SC/六main exact/GPU112/13/80/3256与20保持。旧ratioFAIL/P0缺原baseline未验/null历史FAIL。网页469及spgi-history-source归档6有效capture/真实HDR/原始SHA/失败日志。下一步实际Trace producer SDF停步精修/首面source与认证Gather身份，修空间传播前补实际跨面source/material/可见性，不调sigma/alpha/normalfloor/confidence/曝光光或无证据重推history source；目标未完成。

最新463：已知薄布传输首面资格mode4扩展DISCARD，保留448默认关mode2。核SHA历史448追加frontface前native material拒绝151687全部0x26b300/T8=179/thickness8=38，现行CardCapture alpha0xd9；不是本轮新数/opacity证书。临时mode4只允许已知T/thickness，原SDFSCfailed t0真首面20µm/64stack/identity/frontface/预算/owner/query-key-cacheT保持，actualcapture transport byte exact和triangle2mm+bary后取已有Lo，不改变材质参数/光曝光/压暗SH-confidence/ray更新，也无透射续射证明。新同binary350真实Fusion右−2.629861%、左+1.220893%、墙−0.381553%、近地+0.008128%，实际HDR RGB分量另存，固定EV无明确颜色质感层次收益，回退，不称等价；候选480未跑/新cohort native fetch未逐条认证，不借旧数做新证书。3源+231SPV共234恢复、448244SHA exact/全部dirty保留/mode4生产支持移除/fullserialDebugVulkan实际两cpp重编译/Unit7+Debug5/新mode2V350480/Metal≥65/原SHA SCstage+native全帧六ROI1%及Fusion2halfULP PASS，旧ratioFAIL/P0缺baseline未验/null历史FAIL。网页463及Docs/reports/sc-resolve-2026-10-03/spgi-thin-first保存5新captures/真实HDR/材料范围/原始SHA。继续实际producer原始SDF停步/精修、方向/首面/source radiance与history祖先或缓存支持域缺口，不简单允许所有flags/调T/SH-confidence-normalfloor/光曝光压暗/旧选择门，不无新证据重推mode4或discard的world线段数组。目标未完成。

最新458：原Gather invocation384actual receiver/3409正tap，19同帧GPU源；actualposition/normal/SH逐位、Spatial→Gather exact/原1e−5 evaluation归约和2halfULP输出闭合。右504tap反向N0/max连接0.123360m；历史448 geometry核SHA共同staticasset相等+新native endpoints的float64 CPU段首面右182interior，其中164两端同plane≤原2mm，147同原triangle bary；不可当隔墙拒绝。余18端点支持UNKNOWN，在冻结rawGather正项Y0.708136%，不是最终Fusion/能量/删分支反事实或history根因；历史geometry不是新GPU证书。boundVP float64 inverse残差变小但右147cohort plane0.770981→0.794927mm，不推广矩阵/点修复或重猜矩阵。native/OFF350真实Fusion右+0.134026/左+0.105406%，无新视觉修复；235临时文件移除、448244SHA exact/全dirty保持/actual恢复binary无SPGI_NATIVE_AUDIT；fullserialDebugVulkan/Unit7+Debug5/新mode2V350480/Metal≥65/SCstage+native门PASS，旧ratioFAIL/P0缺baseline未验/null历史FAIL。初次临时layout错误REJECTED已修正，失败保留不冒充有效capture。网页458及Docs/reports/sc-resolve-2026-10-03/native-spgi-gather保留5新captures/actualHDR/原生记录/CPUgeometry范围/原始SHA。下一步producer实际ray origin/stop-refine/first/source radiance与history祖先；若修Gather连接先证同帧端点surface/material/间隔面opacity，不调normal floor/confidence/SH/曝光光源压暗rays或旧选择门、不重推SC4×4世界线段数组。目标未完成。

最新453：正距离首面重试扩展DISCARD，仍保留448可选零距离mode2（默认关）。同binary/SPIRV350，临时mode3允许SDF→SC失败且最终t>0有限值，其余真实首面/material/frontface/capturedtriangle资格保持；真实Fusion右间接HDR−21.0095%、左−4.48651%、墙+0.959727%、近地−1.58815%，有变化不是等价失败，但未见明确颜色/质感/层次收益、新positivecohort native first/source未逐条认证，谨慎回退，候选480未跑。仅回退本轮3源+231SPV共234文件，448累计244SHA exact，保留此前全部dirty；mode3生产支持已移除，禁止借448零距离诊断数做新证书。fullDebugVulkan（-j8双目标copy冲突后-j1成功）actual2cpp recompile/Unit7+Debug5/新回退mode2V350480/Metal≥65/原SHA SCstage/native全帧六ROI1%/Fusion2halfULP通过；旧绝对ratio门仍FAIL，P0原baseline缺失未验/null历史FAIL，不称总体绿。网页453及Docs/reports/sc-resolve-2026-10-03/spgi-positive-stop/README.md/summary.json/raw-manifest.json保存5新captures、真实HDR/决定/原始SHA。下一步留448基础优先暗部颜色/normal/插值及actual history writer→SH→gather/Fusion；若再试正距离资格先闭合actual原SDF停步/精修与真实首面/native fetch身份，不把更暗当收益或根因。目标仍未完成。

最新448：首面owner目录可选KEEP，固定SC环境加PRIMAL_SPGI_ZERO_HIT_GEOMETRY=2（默认关）。仅原SDF→SC失败且最终t=0，BVH真正首面20µm/64stack/identity/同owner Opaque alpha1 graph0/actualflags0/捕获点同原triangle2mm+内部+transport；原格重试失败再owner目录，未知不穿过，生产再加几何front-face门。诊断450699新owner卡不在实际GPU原格最多8候选，native fetch pixel/原1e−5 radiance及triangle资格闭合；是追加front-face前诊断数量，不冒充新生产数量/float64/driver descriptor/opacity证书。清理后的新同binary OWNER/OFF350480，固定EV1.8546/ToneAdapt0真实indirect右−16.8791/−16.3877%、左−13.6015/−13.5815%，墙+9.23/9.19%，截图部分灰亮减少但颜色质感/层次未达Lumen。五source一SPV六文件保留，其余244中238原SHA保持所有dirty；临时SPGI快照/旁路buffers/复制依赖移除，fullDebugVulkan/actualrecompile/Unit7+5/新OWNER OFF350480/Metal≥65/SCstage原baselineSHA PASS；9新Vulkan actualnative全帧六ROI1%/Fusion2halfULP通过。旧绝对截图ratio门OFF/OWNER都FAIL未改/豁免，legacy CPU gather有失败项分列；P0原基线缺失本轮未验/null历史FAIL，不称总体全绿。网页448及Docs/reports/sc-resolve-2026-10-03/spgi-first-surface-owner保存native候选/texel/HDR/生产diff/gzip SHA。下一步继续剩余首面未知/不支持/SC支持缺口和SDF original stop/refine actual来源，闭合暗部邻点normal/插值/history writer→SH→gather/Fusion，优先颜色质感及空间遮挡层次；不能把本次局部灰亮改善说成匹配Lumen或唯一根因，不加bias/删全回退/调confidence-alpha-SH阶数-曝光-光源/压暗或旧选择门，不重推discard世界线段数组。目标未完成。

最新438：实际零距离BVH→SC窄资格修复同binary/SPIRV OFF350/ON350已DISCARD，只有原SDF→SC失败且最终t=0重试真实首面、identity/同owner Opaque/实际flags0及捕获点原triangle2mm资格。新14356SC（13255正权重），native hit/output逐位、CPU所选atlas radiance重放误差0；oldaccepted未重试，加入actual importance SH后2050旧card/key逐位保持/main六binary保持。固定EV1.8546/ToneAdapt0无暗部颜色质感/空间层次改善，真实indirect右+0.30728%、左−0.10046%、近地−0.00150%。五源/231SPIRV236SHA恢复+累计244SHA exact、actual recompile/fullDebugVulkan/Unit7+5、新V350480/Metal≥65/SCstagePASS；P0缺原基线未验/历史FAIL。网页438及Docs/reports/sc-resolve-2026-10-03/spgi-zero-hit-geometry保存实际开关/HDR/源码/raw SHA gzip。CPU扫描全部owner卡在768993失败重试中找到458869有原阈值合格卡，但不是actual spatial-grid枚举/拒绝/opacity证书；BuildCardGrid最多8卡/cell。下一步直接补actual grid offsets/cards/候选读取身份，并只对原失败且真实首面资格通过的分支做owner目录同binary窄对照；验证枚举缺失和画面收益，不放宽旧选择，不简单增bias/删回退/调alpha/confidence/SH阶数/曝光/光源，不重推discard世界线段数组。目标未完成。

最新433：Average原invocation actual48ray（非默认64）/57600slots/23GPU源，original fetched rayData逐位、direction/basis/PDF、SH归约误差0，stats/confidence/scaledSH原1e−5通过。actual SDF world fallback1338151，813588最终hit distance=0（右44242、左116796、近地12089）；distance在optional detail refine之后，不证明原stop、自碰撞、false hit、opacity或屏幕根因。L2同48ray current probe normal离线更接近同射线积分但暗廊略升，非真实图/视觉KEEP，暂不凭此升阶。三source/231SPIRV234SHA恢复+累计241SHA exact、actual recompile/fullDebugVulkan/Unit7+5、新V350480/Metal≥65、旧SC/native gather/Fusion/SCstagePASS；P0缺基线未验/历史FAIL。网页433及Docs/reports/sc-resolve-2026-10-03/native-spgi-average保存固定EV/HDR/实际源与SHA gzip。下一步必须针对原SDF→SC失败且最终t=0世界回退做original stop/refine/receiver真实首面资格及同binary/SPIRV窄修复，旧已接受SC/main card/cacheT保持；不要继续只做矩阵或绑定诊断，不简单增origin bias（薄壁风险）、删全部回退、调confidence/alpha/曝光/光源/压暗或重推discard世界线段数组。目标未完成。

最新429：Temporal原invocation实际48vec4/57600slots与18actual GPU源，native actual IDs/weights/fetchedSH/output逐位身份闭合；36191mix/87265接受tap，原测试1e−5通过max3.814697e−6。本批CPU58probe/max.000284195拒绝改用native读取全过，active接受ID一致，权重有浮点重投影差。不是417旧53同批证书，不是物理history/opacity/灰亮根因。fixed384/Spatial links仍CPU重建。旁路已完整移除234SHA+此前累计240SHA exact、实际重编译/fullDebugVulkan/Unit7+5/新V350480/Metal≥65、原SC/native gather/Fusion/SCstagePASS；P0缺原基线未验/历史FAIL。native/OFF350同binary两feedback runs暗廊indirect−.500312%、近地−.179190%，颜色质感层次无收益、不归因snapshot/积累。网页429及Docs/reports/sc-resolve-2026-10-03/native-spgi-temporal保存固定EV/HDR/native记录与SHA gzip。下一步回到原ray/hit/radiance→Average producer和near早停窄几何资格开关/暗部贡献，不能继续猜Temporal矩阵或重测同四条post依赖，不调alpha/normal floor/confidence/曝光/光照/额外压暗、减少ray或放宽旧选择/重推discard世界线段数组。目标未完成。

最新424：SPGI post四条明确Compute buffer依赖，同binary/231SPIRV/static assets OFF/ON/ON/OFF四350，仅加同步，原ray/SH估计/权重/EV保持，原SC四审计/六main/GPU/native gather/Fusion全部PASS。两对暗廊indirect−.5003/−.1341%、墙−.4074/−.0928%、近地−.1803/−.0640%，ON普通重复亦变化，固定EV未改善颜色质感/空间层次，DISCARD。232SHA完整恢复+此前共239SHA exact，touch后实际重编译/fullDebugVulkan/Unit7/新V350480/Metal≥65/SCstagePASS；P0缺原基线未验/历史FAIL。原数据/ABBA/HDR/SHA gzip见Docs/reports/sc-resolve-2026-10-03/spgi-post-sync-candidate。下一步native Temporal实际tap IDs/weight/SH读取与producer→SH资格，闭合417的53probe/281component CPU拒绝；不要再重复矩阵变体或同四条同步边，不改权重/曝光/光照或重推discard。目标仍未完成。

最新417：SPGI单帧17实际GPU源/57600slots，Spatial→Gather SH/denoise→Fusion byte exact；固定384 receiver/taps CPU重建及两halfULP重放，不等于native selection/物理遮挡证书。Temporal CPU53probe/281component/max.000296831 REJECTED，矩阵变体均未闭合，不放宽；native actualtap/weight仍UNKNOWN。保存同binary/231SPV快照ON/OFF与重复OFF，camera/depth/normal/origins/gather receiver/旧SC保持，但SH/GI都非exact，不能把ON/OFF差全部归因快照或拼跨run因果。现行SPGI Average→Temporal→Spatial→Gather主要仅InsertBarrier，而VK buffer跳过；下一步先同binary/SPV窄开关验证这些明确compute依赖，保持ray/radiance/旧SC，再补native Temporal/producers。两源/231SPV恢复+此前共239SHA exact，纠正copy2旧mtime后实际重编译/fullDebugVulkan/Unit7/新V350480/Metal≥65/SCstagePASS；P0原基线缺失未验/历史FAIL。原数据/拒绝与SHA gzip在Docs/reports/sc-resolve-2026-10-03/native-spgi-chain。视觉未达Lumen。

最新411阶段：新生产350固定EV1.8546/ToneAdapt0，237SHA/binary/assets保持；四SC/六main/GPU及native gather/Fusion原门PASS。当前diffuse half913431选near/8169无级，far直接贡献0，regular/low同一near SH；最终右暗廊regular40.56%、low16.53%、SPGI42.97%。只是当前有符号分解，未证物理根因或视觉改善。410另一actual场仍UNKNOWN，但不应作为当前直接diffuse首要方向；下一步near实际插值连接几何/材质资格及SPGI producer→temporal/spatial SH→gather身份，不调flag/blend/confidence或额外压暗。411原输入/HDR及SHA gzip在Docs/reports/sc-resolve-2026-10-03/native-gather-split；410 fullbuild/Unit7/V350480/Metal/SCstagePASS是历史回归，P0缺基线未验/历史FAIL。

最新410阶段：近级225240 actualfetch＋同次完整384³R16F/cache及native pipeline/descriptor0/dispatch/HN/update身份闭合，原48同批source/stop/fetch保持；另一actual级用不同runtime GlobalSDF，未由near证书认证。源码offline只转near、signed flag传播全部，不能未经actualCB/texel验证就改flag/blend或宣称符号错误。下一步独立另一级同dispatch field/CB/stop/fetch→SH→regular-low gather→暗部Fusion。六原dirty源＋231SPIRV共237SHA恢复/fullDebugVulkan/Unit7/新V350480/Metal≥65秒、四SC六mainGPU/3256与20及SCstagePASS；P0缺基线未验/历史FAIL。近级诊断main351/352原350门拒绝；初次all-pipeline同场断言拒绝并保留。原数据在Docs/reports/sc-resolve-2026-10-03/native-sdf-identity/raw-manifest.json。禁止重推discard或放宽门；仍未达Lumen。

最新续作状态（406阶段，先读交接顶部）：联合真实首面点/ρ/mappedN＋raw或E/π历史两候选均DISCARD。当前572 ray实际资格/native读取/历史重放闭合，暗廊−8.306/−10.346%但墙−25.427/−26.006%，视觉未达Lumen。六原dirty源码＋231SPIRV共237SHA完整恢复、fullDebugVulkan/Unit7/新V350480/Metal≥65秒、四SC六mainGPU/3256与20保持，SCstagePASS；P0缺基线未验/历史FAIL。候选main351/352被原350门拒绝，三组互相exact不替代原证书。原始数据见Docs/reports/sc-resolve-2026-10-03/analytic-joint-candidate/raw-manifest.json。下一步实际粗场native descriptor/sample来源和暗部接收点source/history资格，区分conditioning UNKNOWN/mesh支持域，不能重推此次联合或之前discard修复、放宽门或删coldstart。

在 `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP` 继续 SC／两层 BVH 与间接光优化，直接排查、修改、验证和按证据保留／回退，不停留在方案讨论。

先读 AGENTS.md、Docs/ARCHIVE.md，然后读：
- Docs/completed/2026-10-05-sc-bvh-lumen-optimization-handoff.md（本次交接主入口）
- Docs/completed/2026-09-30-sc-per-mesh-surface-bake-and-lookup-implementation.md（顶部最新记录）
- Docs/completed/2026-09-30-sc-per-mesh-data-format-spec.md
- Docs/RHI/GraphicsAPI_Parity_Matrix.md、Docs/rhi-barrier-semantics.md
- Docs/completed/2026-10-05-unreal-lumen-pass-analysis.md（来自 codex://threads/01a10510-a2d4-7343-9082-36770e414011）
- Docs/reports/sc-resolve-2026-10-03/native-sample37/README.md 和 summary.json。

优先改善暗部间接光、颜色与质感、空间层次；场景、材质、构图差异暂不作为主方向。漏光、积累和不连续是待验证假设。不要调曝光、光照、额外压暗或改门禁，不减少射线或跳帧复用旧radiance。每阶段保留固定EV1.8546截图和真实间接HDR，在现有376阶段网页对照用户Lumen图解释变化，命中数与Debug性能不能代替视觉改善。

当前可选SC主resolve 8190条真实射线正确命中3256、20旧地面缺口恢复，旧card/cacheT严格保持。最新37N实际last sample诊断已完整回退，生产五源／12变化模块／231SPIR-V恢复；完整DebugVulkan构建、Unit7、Vulkan350/480与Metal≥65秒回归通过。SCstage PASS；P0原基线缺失本轮未验，历史FAIL，不能混用。

调查确认：同批1159实际射线的GPU／float64首面证据（20µm／64stack）与原SDF停步对应；932早停、76晚停、151无50m首面。37N保留完整16候选；433cross早停中429实际最后sample在首面前，其3432角点全0且零候选仅为mesh支持域外AABB下界，native scalar全0。它不是“BVH命中而SDF没有命中”，而是SDF可能在离面位置先停。尚未证明该机制导致暗部偏亮；CPU角点也不等于native fetch／descriptor证书。

下一步从实际sample粗场来源与native读取身份补证据，尝试窄范围几何资格修复的同binary/SPIR-V开关对照，并闭合暗部邻像素实际来源／回退、法线方向、插值范围和history writer→SH→gather／Fusion贡献。不得凭UE流程或局部遮挡就宣称根因。无收益或等价失败自行回退；不得重引入已discard的4×4线段世界坐标数组、放宽旧选择保持。

按交接文档固定环境运行；只构建Darwin/DebugVulkan（与Debug共享libEngine.a），不交叉污染；保留全部既有dirty工作。每次保留／回退后做适当回归并更新实现记录与ARCHIVE。GPU BOTTOM_OF_PIPE是命令标记间隔，不能叫独占kernel时间。

临时sc-*中间结果已清理。持久基线、capture、文本历史在Docs/reports/sc-handoff-2026-10-05；关键raw由各阶段raw-manifest.json及gzip恢复，使用scripts/restore_sc_stage_raw.py。先核SHA或重捕获，不把不存在的临时文件或旧结果当新验证。UE16.69GiB原始归档与376阶段截图网页保留。更多精确条件、未知项和工具入口以交接文档为准。
