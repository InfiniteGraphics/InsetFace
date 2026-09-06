# InsetFace 几何算法技术文档

本文档描述 [InsetFace.cpp](D:\myfiles\coding\cpp\mqsdk485\mqsdk\InsetFaceRepo\plugin\InsetFace.cpp) 中与几何处理直接相关的核心算法、数据结构、数学模型、数值保护策略，以及从预览到提交的完整执行路径。

## 1. 设计目标

`InsetFace` 要解决的不是单一“把边往里缩”的问题，而是一组相关问题：

- 允许用户对一个面或一组连通面做 inset
- 支持局部厚度 `Thickness`
- 支持法线方向位移 `Depth`
- 支持“普通 inset”和“均等偏移（Offset Even）”两种厚度语义
- 在预览阶段尽量稳定，不轻易炸掉面片
- 在提交阶段生成新的内盖面和侧壁面，并删除原始选中面

当前实现分成两条主要几何路径：

- `Individual`
  每个面独立在自己的局部平面中 inset
- `Region`
  把连通选中面作为一个整体处理

## 2. 关键数据结构

### 2.1 参数

`InsetParameters`

- `Thickness`
- `Depth`
- `EvenOffset`
- `CurrentMode`

其中 `EvenOffset` 决定厚度到底按“顶点位移”解释，还是按“边到边真实距离”解释。

### 2.2 临时区域拓扑

`TempRegion` 是区域级处理的核心容器，内部包含：

- `Vertices`
- `HalfEdges`
- `Faces`
- `BoundaryLoops`
- `PlaneOrigin`
- `PlaneU`
- `PlaneV`
- `PlaneNormal`

其中：

- `TempVertex` 保存原顶点、目标顶点、平均法线、二维投影位置
- `TempHalfEdge` 保存半边拓扑连接关系
- `TempFace` 保存原始面、材质、局部 inset 结果
- `TempLoop` 表示区域边界环或孔洞边界

这套结构让插件可以在“预览阶段”先完成几何推演，再在“提交阶段”统一写回对象。

## 3. 数学基础

### 3.1 二维向量运算

代码里定义了基础二维点 `Point2D`，并使用如下运算：

点积：

$$
\operatorname{dot}(\mathbf{a}, \mathbf{b}) = a_x b_x + a_y b_y
$$

二维叉积标量：

$$
\operatorname{cross}(\mathbf{a}, \mathbf{b}) = a_x b_y - a_y b_x
$$

向量长度：

$$
\|\mathbf{v}\| = \sqrt{\operatorname{dot}(\mathbf{v}, \mathbf{v})}
$$

左法向：

$$
\operatorname{perpLeft}(x, y) = (-y, x)
$$

归一化：

$$
\widehat{\mathbf{v}} = \frac{\mathbf{v}}{\|\mathbf{v}\|}
$$

### 3.2 多边形有向面积

二维多边形面积通过 shoelace 公式计算：

$$
A(P) = \frac{1}{2}\sum_{i=0}^{n-1} \operatorname{cross}(\mathbf{p}_i, \mathbf{p}_{i+1})
$$

其中 $\mathbf{p}_n = \mathbf{p}_0$。

作用：

- 判断面是否退化
- 判断顶点顺序方向
- 生成 inward normal 时决定偏移朝向

### 3.3 三维面法线

单面法线使用一个 Newell 风格的离散累积公式：

$$
n_x += (y_i - y_{i+1})(z_i + z_{i+1})
$$

$$
n_y += (z_i - z_{i+1})(x_i + x_{i+1})
$$

$$
n_z += (x_i - x_{i+1})(y_i + y_{i+1})
$$

最后归一化得到：

$$
\widehat{\mathbf{n}} = \frac{\mathbf{n}}{\|\mathbf{n}\|}
$$

这一步在 `ComputeInsetPolygon()` 里用于建立单面的局部二维坐标系。

## 4. 从三维面到二维局部坐标

对于单个面，插件先构造局部平面基底：

- 原点：$\mathbf{o} = \mathbf{p}_0$
- 第一基向量：从边方向中取一条，投影到面平面内
- 第二基向量：

$$
\mathbf{v} = \widehat{\mathbf{n}} \times \mathbf{u}
$$

任意三维点 $\mathbf{p}$ 被投影到二维：

$$
\mathbf{q} =
\begin{bmatrix}
(\mathbf{p} - \mathbf{o}) \cdot \mathbf{u} \\
(\mathbf{p} - \mathbf{o}) \cdot \mathbf{v}
\end{bmatrix}
$$

反投影回三维时：

$$
\mathbf{p}' = \mathbf{o} + q_x \mathbf{u} + q_y \mathbf{v} + d \widehat{\mathbf{n}}
$$

其中 $d$ 是 `Depth`。

## 5. 单面 inset：`ComputeInsetPolygon()`

`ComputeInsetPolygon()` 的职责是：

1. 计算单面法线
2. 把三维多边形投影到二维
3. 调用 `OffsetLoop2D()` 得到内缩后的二维轮廓
4. 把结果抬回三维
5. 生成内盖面的 UV

### 5.1 UV 处理

当前实现对内盖 UV 使用一种简单中心收缩近似：

设原 UV 为 $\mathbf{t}_i$，UV 中心为：

$$
\mathbf{c}_{uv} = \frac{1}{n}\sum_{i=0}^{n-1}\mathbf{t}_i
$$

则新的内层 UV 为：

$$
\mathbf{t}'_i = \mathbf{t}_i + 0.5(\mathbf{c}_{uv} - \mathbf{t}_i)
$$

这不是严格保面积或保边长的 UV 参数化，只是稳定且足够便宜的近似。

## 6. 轮廓偏移：`OffsetLoop2D()`

这是整个插件里最核心的二维几何步骤。

输入：

- 原始二维闭环 `loop_points`
- 有符号偏移距离 `signed_thickness`
- `even_offset` 开关

输出：

- 偏移后的闭环 `out_points`

### 6.1 方向与 inward normal

先通过面积符号得到多边形方向：

$$
\operatorname{orientation} =
\begin{cases}
1, & A(P) \ge 0 \\
-1, & A(P) < 0
\end{cases}
$$

设当前顶点为 $\mathbf{p}_i$，相邻边方向为：

$$
\mathbf{d}_0 = \widehat{\mathbf{p}_i - \mathbf{p}_{i-1}}, \quad
\mathbf{d}_1 = \widehat{\mathbf{p}_{i+1} - \mathbf{p}_i}
$$

则 inward normal 定义为：

$$
\mathbf{n}_0 = \widehat{\operatorname{perpLeft}(\mathbf{d}_0) \cdot \operatorname{orientation}}
$$

$$
\mathbf{n}_1 = \widehat{\operatorname{perpLeft}(\mathbf{d}_1) \cdot \operatorname{orientation}}
$$

### 6.2 普通 inset：`even_offset = false`

普通模式下，不追求“边到边距离严格等于 thickness”，而是把顶点沿角平分线近似内移。

定义角平分方向：

$$
\mathbf{b} = \widehat{\mathbf{n}_0 + \mathbf{n}_1}
$$

则新顶点近似为：

$$
\mathbf{p}'_i = \mathbf{p}_i + t \mathbf{b}
$$

其中 $t = \text{signed\_thickness}$。

这意味着用户输入的 `Thickness` 在普通模式下更接近“顶点收缩量”，而不是“真实边距”。

设内角为 $\theta$，则对应真实边距大致变成：

$$
d_{\text{edge}} \approx t \cos\left(\frac{\theta}{2}\right)
$$

因此：

- 钝角处真实边距会比 `Thickness` 更小
- 锐角处视觉收缩更强

这正是普通 inset 和 even offset 的关键差异。

### 6.3 均等偏移：`even_offset = true`

均等偏移模式下，目标是让新轮廓到原始相邻边的距离更接近固定值 $t$。

对两条相邻边分别构造平移后的直线：

$$
L_0(s) = (\mathbf{p}_i + t \mathbf{n}_0) + s\mathbf{d}_0
$$

$$
L_1(r) = (\mathbf{p}_i + t \mathbf{n}_1) + r\mathbf{d}_1
$$

新顶点取两条偏移边的交点，即解：

$$
(\mathbf{p}_i + t \mathbf{n}_0) + s\mathbf{d}_0 =
(\mathbf{p}_i + t \mathbf{n}_1) + r\mathbf{d}_1
$$

代码里通过二维无限直线求交实现这一步。

这是更标准的 polygon offset 语义。对于非退化凸角，交点同时满足：

$$
\operatorname{dist}(\mathbf{p}'_i, E_{i-1}) \approx t
$$

$$
\operatorname{dist}(\mathbf{p}'_i, E_i) \approx t
$$

因此用户感知会更接近 Blender 一类 DCC 里的 `Offset Even`。

### 6.4 平行边和尖角回退

如果两条偏移线接近平行，求交会失败。这时实现使用角平分方向作为 fallback：

$$
\mathbf{b} = \widehat{\mathbf{n}_0 + \mathbf{n}_1}
$$

并估计 miter 长度：

$$
\ell = \frac{t}{\mathbf{b} \cdot \mathbf{n}_1}
$$

得到：

$$
\mathbf{p}'_i = \mathbf{p}_i + \ell \mathbf{b}
$$

但过长的 miter 会造成尖刺，因此实现引入上限：

$$
|\ell| \le |t| \cdot M
$$

其中：

- 普通模式 `M = 8.0`
- even offset 模式 `M = 12.0`

这就是代码中 `kMaxMiterFactor` 及其 even 分支的含义。

### 6.5 稳定性检查

候选轮廓生成后，还会进行三类检查：

1. 面积不能退化到零
2. 偏移后轮廓方向不能反转
3. 轮廓不能发生自交

若失败，则把偏移距离缩小一半重试：

$$
t_{k+1} = 0.5 t_k
$$

最多尝试 8 次。若仍失败，则给出 `WarnCollapsed`。

这是一种简单但非常实用的数值保护策略。

## 7. `Individual` 模式

`Individual` 模式通过 `BuildFaceLocalInset()` 为每个面分别构造局部 inset。

逻辑上，每个面都独立执行：

1. 取出该面的顶点和 UV
2. 在本地平面中调用 `ComputeInsetPolygon()`
3. 保存该面的 `LocalInsetPoints`

提交时，新内盖面和侧壁都来自这些局部结果。

优点：

- 对单个面稳定
- 对局部非共面的情况更自然
- 与传统逐面 inset 直觉一致

缺点：

- 多面共享顶点时，单独计算的内缩结果并不自动一致
- 因此需要下一步的“共享顶点调和”

## 8. `Region` 模式的普通路径：`SolvePatchInsetVertices()`

在 `Region` 且 `EvenOffset = false` 时，算法不是直接对区域边界做统一二维偏移，而是先做“每面局部 inset”，再把共享顶点的切向偏移合并。

### 8.1 每个面的局部偏移

每个面先独立得到本地 inset 顶点：

$$
\Delta \mathbf{x}_{f,v}^{local} = \mathbf{x}_{f,v}^{inset} - \mathbf{x}_v
$$

### 8.2 投影到顶点平均法线的切平面

为了避免多个面法线不一致时互相拉扯，把局部位移投影到顶点平均法线对应的切平面：

$$
\Delta \mathbf{x}_{f,v}^{tan}
=
\Delta \mathbf{x}_{f,v}^{local}
-
\left(
\Delta \mathbf{x}_{f,v}^{local} \cdot \widehat{\mathbf{n}}_v
\right)\widehat{\mathbf{n}}_v
$$

### 8.3 按角度加权平均

顶点在不同面中的影响权重由角点角度近似给出：

$$
w_{f,v} \approx \angle(\mathbf{e}_{prev}, \mathbf{e}_{next})
$$

最终切向位移：

$$
\Delta \mathbf{x}_v^{tan}
=
\frac{\sum_f w_{f,v}\Delta \mathbf{x}_{f,v}^{tan}}
{\sum_f w_{f,v}}
$$

最终新顶点位置：

$$
\mathbf{x}'_v
=
\mathbf{x}_v + \Delta \mathbf{x}_v^{tan} + d\widehat{\mathbf{n}}_v
$$

其中 $d$ 是 `Depth`。

### 8.4 这个方法的意义

它本质上是在做一种“多面局部结果的切向协调”，优点是：

- 不要求整片区域严格共面
- 计算代价低
- 预览稳定

但它不是严格的整体边界 offset 解，所以对复杂区域的“均匀性”不如真正的区域 even offset。

## 9. `Region` 整体路径

当 `CurrentMode == Region` 时，实现先建立统一的区域级几何上下文，然后在“近共面二维路径”和“非共面曲面感知路径”之间自动切换；`EvenOffset` 只改变厚度语义：

1. `FitLocalPlane()`
2. `IsRegionNearlyPlanar()`
3. near planar:
   `ProjectRegionVertices() -> ExtractBoundaryLoops() -> SolveRegionInterior2D()`
4. nonplanar:
   `BuildSurfaceAwareBoundaryTargets() -> SolveRegionInterior3D()`
5. 两条 Region 路径都在区域层面统一减半厚度重试；若仍失败则返回 warning，不提交该区域

### 9.1 局部平面拟合：`FitLocalPlane()`

区域平面原点取所有区域顶点的质心：

$$
\mathbf{c} = \frac{1}{N}\sum_{i=1}^N \mathbf{x}_i
$$

法线取各个面法线按面积加权平均：

$$
\mathbf{n}
=
\sum_f A_f \widehat{\mathbf{n}}_f
$$

然后归一化得到区域平面法线。

平面基向量 `PlaneU` 取区域内最长边方向投影到该平面后的结果，`PlaneV` 再由叉积得到。

这不是 PCA 最佳拟合平面，但实现简单、稳定，且和网格主边方向更一致。

### 9.2 平面性判定：`IsRegionNearlyPlanar()`

区域是否继续走统一二维 offset，不只看法线，还同时看几何离面量。

设：

$$
t_{eff} = \max(|t|, 10^{-4})
$$

实现统计：

- `max_vertex_plane_distance`
- `max_face_normal_deviation_deg`

仅当满足：

$$
\text{max\_vertex\_plane\_distance} \le 0.25\, t_{eff}
$$

以及：

$$
\text{max\_face\_normal\_deviation\_deg} \le 8^\circ
$$

时，区域才会被视为 nearly planar。

### 9.3 近共面路径：二维区域 offset

如果区域被判定为近共面，则继续走原有二维区域 even-offset 方案。

#### 9.3.1 顶点投影：`ProjectRegionVertices()`

每个顶点被投影到区域二维平面：

$$
\mathbf{q}_i =
\begin{bmatrix}
(\mathbf{x}_i - \mathbf{c}) \cdot \mathbf{u} \\
(\mathbf{x}_i - \mathbf{c}) \cdot \mathbf{v}
\end{bmatrix}
$$

#### 9.3.2 边界提取：`ExtractBoundaryLoops()`

区域拓扑基于半边结构构建。

一条半边若没有 `Pair`，说明它位于区域边界上。算法会：

- 收集所有边界半边
- 按顶点连接关系追踪闭环
- 为每个闭环计算面积
- 面积小于 0 的环视为孔洞

因此区域 even offset 支持：

- 外边界
- 孔洞边界

#### 9.3.3 边界 even offset

对每个边界环，调用：

$$
\texttt{OffsetLoop2D(loop, signed\_thickness, true)}
$$

如果是孔洞，则厚度取反：

$$
t_{hole} = -t
$$

这样孔洞边界会向正确方向偏移。

#### 9.3.4 内部点求解：固定边界迭代平均

边界顶点先被固定到偏移后的二维位置。对内部顶点，当前实现使用固定次数的邻接平均迭代：

$$
\mathbf{q}_i^{(k+1)}
=
\frac{1}{|\mathcal{N}(i)|}
\sum_{j \in \mathcal{N}(i)} \mathbf{q}_j^{(k)}
$$

其中：

- $\mathcal{N}(i)$ 是顶点 $i$ 的邻接点集合
- 边界顶点不参与更新

这可以看成一个离散 Laplace 方程的 Jacobi 型近似求解器。

#### 9.3.5 抬回三维并施加深度

内部二维位置解出后，再转回三维：

$$
\mathbf{x}'_i
=
\mathbf{c} + q_{i,x}\mathbf{u} + q_{i,y}\mathbf{v} + d\widehat{\mathbf{n}}^{avg}_i
$$

这里的深度方向用的是顶点平均法线，而不是区域单一法线，这样在弯曲区域上视觉上更自然。

### 9.4 非共面路径：曲面感知区域 offset

如果区域被判定为 nonplanar，则不再把整块区域压到一个全局二维平面里，而是直接在三维里解切向 inset。

#### 9.4.1 边界目标构造：`BuildSurfaceAwareBoundaryTargets()`

非共面路径只读取区域边界半边和相邻选中面的法线，不调用 `BuildFaceLocalInset()`。边界半边方向为 $\mathbf{t}$，相邻面法线为 $\mathbf{n}$，面内方向为 $\operatorname{Normalize}(\mathbf{n}\times\mathbf{t})$。

每个边界顶点用前后两条边建立三个约束：$\operatorname{dot}(\mathbf{d},\mathbf{inward}_0)=t$、$\operatorname{dot}(\mathbf{d},\mathbf{inward}_1)=t$、$\operatorname{dot}(\mathbf{d},\mathbf{n}_v)=0$。约束接近退化时使用 inward 平均方向，并限制 miter 长度不超过 $8|t|$。

这一步保留的是折角处的局部切空间信息，而不是单平面投影后的结果。

#### 9.4.2 三维内部点求解：`SolveRegionInterior3D()`

内部点直接解三维切向位移，边界点固定为上一节的目标，然后使用反边长加权的邻接平均迭代：

$$
\Delta \mathbf{x}_i^{(k+1)}
=
\Pi_{T_i}
\left(
\frac{\sum_{j\in\mathcal{N}(i)} w_{ij}\Delta \mathbf{x}_j^{(k)}}
{\sum_{j\in\mathcal{N}(i)} w_{ij}}
\right)
$$

其中 $w_{ij}=1/|e_{ij}|$，$\Pi_{T_i}$ 表示重新投影到顶点 $i$ 的切平面。迭代在位移变化小于模型尺寸的 $10^{-6}$ 或 300 次后停止。

最终位置为：

$$
\mathbf{x}'_i
=
\mathbf{x}_i
+ \Delta \mathbf{x}_i^{tan}
+ d\widehat{\mathbf{n}}_i
$$

#### 9.4.3 区域级校验与回退

每次求解后检查所有选中面的法线方向、边长和坐标有限性。失败时整个区域统一将厚度减半重试，最多 24 次；这样圆角窄面不会单独改变厚度而破坏边界连续性。

## 10. 预览与提交

### 10.1 预览

预览阶段通过 `RebuildPreview()` 完成：

1. 按对象收集选中面
2. 把连通面划分成多个 face group
3. 对每个 group 调用 `BuildPreviewForFaceGroup()`
4. 得到 `TempRegion`
5. 抽取预览线并存入 `PreviewMesh`

预览线主要包括：

- 内层轮廓边
- 原边界顶点到新边界顶点的连线

### 10.2 提交

`ApplyPreview()` 调用 `ApplyCommitDataToObject()` 真正写回网格：

1. 复制区域中的所有新顶点
2. 用这些新顶点创建内盖面
3. 对区域边界的每条半边创建一个四边形侧壁
4. 删除原始选中面
5. `Compact()`
6. `UpdateNormal()`

侧壁四边形顶点顺序为：

$$
[\text{orig}_{start}, \text{orig}_{end}, \text{new}_{end}, \text{new}_{start}]
$$

这样侧壁自然连接原边界与新内边界。

## 11. 数值保护与 warning 体系

当前实现对以下问题会给 warning 或直接回退：

- 零面积面
- 退化边
- 边界提取失败
- 非流形边
- 偏移后自交
- 偏移过深导致塌缩
- 平行边 fallback
- 曲面感知路径失败后的普通 patch fallback

核心思路不是“强求数学最优”，而是“在 DCC 交互里尽量稳定地给出一个合理结果”。

## 12. 当前实现的限制

### 12.1 `Individual` 普通模式不是严格 edge-distance offset

未开启 `Offset Even` 时，厚度更接近“顶点沿角平分线移动量”，不是严格的“边到边距离”。

### 12.2 非共面 `Region` 仍是近似曲面解

明显弯曲区域虽然不再强制压到一个全局二维平面，但当前 nonplanar 路径仍不是严格 geodesic offset。它更接近：

- 基于原网格局部切空间的边界约束
- 带固定边界的三维切向平滑传播

因此在强曲率、强不规则三角化、或拓扑较差的区域上，均等性仍然是近似的。

### 12.3 内部点求解不是严格最优

`SolveRegionInterior2D()` 保留固定迭代；非共面 `SolveRegionInterior3D()` 使用反边长加权迭代，最多 300 次并带收敛阈值。两者都没有求解：

$$
\Delta \mathbf{q} = 0
$$

对应的精确稀疏系统，也没有 cotangent 权重。优点是轻便、稳定，缺点是理论最优性不足。

### 12.4 UV 仍是近似方案

内盖 UV 只是向中心线性收缩，尚未实现：

- 边长保持
- 面积保持
- 角度保持

之类更高质量的参数化。

## 13. 后续可改进方向

### 13.1 更精确的区域内部求解

可以把当前平均迭代替换成显式线性系统：

$$
L\mathbf{x} = \mathbf{b}
$$

其中边界为 Dirichlet 条件，内部点求 harmonic map。

### 13.2 更强的非共面区域处理

可以考虑：

- 局部参数化
- 分块平面
- 基于原网格切空间的约束求解

以进一步减少大曲率区域上的近似误差。

### 13.3 更好的 UV 生成

内盖和侧壁可以根据真实偏移路径重建 UV，而不是当前这种轻量近似。

### 13.4 更严格的自交处理

现在的策略是“检测到问题就缩小厚度重试”，后续可以进一步引入：

- 局部裁剪
- offset graph 修补
- 更完整的 polygon clipping

## 14. 结论

`InsetFace` 当前的几何实现可以概括为：

- 单面路径：局部平面内做 polygon inset
- 普通区域路径：多面局部 inset 后做切向协调
- 区域 even 路径：近共面时走二维统一 offset，非共面时走曲面感知边界约束 + 三维切向平滑

它不是“最学术最完整”的 inset 求解器，但它在插件交互场景下实现了一个很好的平衡：

- 足够稳定
- 足够快
- 对常见建模操作足够直观
- 具备继续向更严格几何算法演进的清晰结构
