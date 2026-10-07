# M6-A：运动矢量与 jitter 软件增量

日期：2026-10-06。选择和坐标合同见 [实施决定](M6_A_Motion_Decision.md)。此增量已经有真实运动附件和提交历史；TAA颜色历史、重投影resolve、拒绝/clamp仍是M6-B，GTAO仍是M7。默认画面不启用jitter。

## 已接入的行为

`TemporalMotionHistory`对prepare保持只读，queue submit成功后才commit。Renderer按完整对象集合保存objectIndex/mesh/material/模型矩阵，visible集合必须与完整集合一致。同网格实例分别记录，隐藏物体仍保存当前矩阵；移除后再出现及mesh/materialID改变没有旧物体历史。旧兼容draw接口的匿名物体输出无效历史；SceneEcs/Application正常路径提供明确索引。

Frame UBO由448增至608B，旧字段偏移保持，追加当前/前帧未抖动VP、前帧相机/有效位、当前/前帧jitterUV。每槽独立的binding10只供vertex读取：80B/实例previousModel+flags，entry0为无历史，增长/rebind发生在该槽fence完成后。push保持128B，顶点借用shadowPass.y作为索引。fragment仍是16samplers、4storage buffers/22总resources；整个pipeline layout需5个storage额度，在GPU资源创建前检查，额度不足明确拒绝；现有13条pipeline数量保持。增加的顶点varying在最低组件额度内。

主绘制MRT第二附件RGBA16F：xy为currentUV-previousUV，z有效度，w前帧device depth。与PBR共享深度、镜像/双面剔除和alpha discard。MASK孔洞保留背后的运动；新/投影无效的表面强制无历史。BLEND的xy/w不可信，z按覆盖降低，M6-B必须据此限制积累。debug线也降低有效度；UI在显示转换之后，不写场景运动。

Graph按pass uses顺序绑定至多4个颜色附件，各目标独立的写入、内容/清除、依赖和导出屏障，单队列共享HDR/depth/motion/shadow。1080p新增运动像素载荷16,588,800B；真实分配记录由VMA/账本给出，不是VRAM或帧预算结论。resize构造完整候选资源成功后替换，且清历史。

8相位Halton(2,3)居中像素jitter只改变clip.xy/w对应偏移。运动本身去除jitter；未来resolve使用`currentRasterUV - motion + previousJitterUV - currentJitterUV`。CSM拟合坚持未抖动VP；cluster深度轴坚持未抖动相机，格子射线匹配抖动逆VP；剔除留半像素边界。天空只计算相机旋转，平移不会让无限远环境移动。探针使用临时第二附件、无jitter/无历史，不发布正常frame ID或历史。场景提交、camera index切换、Reset Camera、resize、jitter开关和显式reset清历史。

UI：PBR Debug新增`Motion UV`与`Motion History Validity`；Render Debug新增`Jitter preview (TAA pending)`、reset、camera history/jitterUV状态。jitter开关默认关闭，开启预览会抖动，不提供抗锯齿resolve。报告schema4追加motion/jitter/history/TAA字段，目标策略改为`shared_hdr_depth_motion_shadow`，`taa_enabled=false`。

## 验证证据

- CPU：Halton范围/周期、clip jitter/UV符号、保守剔除边界、未提交不前进、重排的同网格实例、移除/身份复用、mesh/material/extent/jitter/cut失效、重复身份与奇异相机拒绝。Motion与Graph的ASan+UBSan通过；四个修改的SPIR-V通过vulkan1.3校验。
- GPU单帧/双帧：首帧无效；静止(0,0,1,.5)；物体X和相机Y平移分别+.05UV；镜像非均匀缩放与解析投影吻合；MASK孔洞保留背面，覆盖时前帧depth=.4；BLEND有效度=.5；jitter静止速度0、CSM四矩阵不动；天空平移0/旋转非零；完整列表中屏幕外实例返回时拒绝越界previous UV，下一静止帧恢复；重复身份在acquire前拒绝且提交ID/采样序列不变，之后可重试；capture不改ID/采样，相机/scene/resize失效。jitter下真正激活的full/clustered光照读回一致(1.27246,1.02246,.897949)，格子逆VP与raster匹配。
- 两个timeline阻塞槽：先槽1后槽0，分别首帧无效与+.1UV；前帧数据由提交顺序决定，160/320B对象缓冲和608B uniform独立；十个完成回调准确且latest ID不回退。原有两槽light/cluster/CSM与场景/UI退休仍通过，最终资源归零。
- 第一次新增jitter-light断言失败是固定取样坐标落在模型外；改为投影模型中心后验证有真实直接光贡献且两路径一致。MRT纯编译测试在旧Graph确实报“one color”，增加多附件后两个目标的依赖/屏障/内容通过；>4附件拒绝。

完整31/31通过（18CPU/13软件GPU），430.35s；随后补充报告字段及屏幕外重入/mesh复用用例，针对受影响项7/7通过55.71s；最后增加创建前的资源额度检查，单/双帧motion与双帧ImGui异步场景3/3通过30.80s。全量渲染验收指纹`c1bedfc4ef680a9186167f9ee380e98a0142cc83ce85cf036a778ac01ed188cf`，最终生产指纹`6afe5228b3702715099a11f2768be2ea24e49722d2ddb92c4967d354beb3f9ac`，均99inputs；中间仅报告收集/序列化和资源额度guard变化，shader、运动历史、管线、graph与缓冲实现未变。Source manifests及完整/针对日志保留在聊天工作目录`outputs/m6a`。带MRT的普通软件transaction用例74–81s，CTest上限90→120s只提供共享机器余量，不是性能验收。

最终两个640x480 viewer烟测：厨房默认预览1slot/static有4行、四合院2slots/orbit有4行，8个GPU查询全部有效；前帧状态有效、四级CSM、motion启用/TAA和jitter关闭，厨房2spot+probe保持。报告指纹与最终生产一致，present release proven，prepared/retired/staging所有计数归零。更新后的benchmark脚本另在报告增量指纹上通过1行/查询/报告/关闭检查。所有GPU结果使用显式llvmpipe/X11及`MESA_VK_WSI_DEBUG=sw,noshm`。软件GPU结果不宣称4060 Ti性能、原生MIT-SHM/DRI3、validation layer或最终移动画质验收。硬件/RenderDoc仍按用户要求等待设备切换。

## 下一项

M6-A的w仍是FP16 device depth，仅作诊断：小nearPlane/远景时不能直接用它判遮挡显露。M6-B必须先改为足够精度的前帧线性深度（并与历史深度采用同一度量），再建立拒绝阈值；不能靠放宽阈值隐藏量化错误。

M6-B在显示转换之前解析线性HDR：历史颜色/深度、reprojection和UV越界/previous-depth/无历史拒绝，邻域clamp与运动权重；UI保持后绘制。先保证静止收敛与移动/显露不拖影，再M6-C透明/灯光/曝光和镜面细节调优，之后M7 GTAO。现在的速度只是几何轨迹，不能证明随灯光和材质变化的颜色历史可信。
