# 游戏引擎技术文档索引

## 概述

本文档目录包含了游戏引擎项目的完整技术文档，涵盖架构设计、API参考、测试规范和开发指南。

**项目版本**: v0.2.0 - 核心基础设施完成  
**更新时间**: 2025-12-29  
**文档版本**: 1.0.0  

---

## 文档分类

### 📋 规划与设计文档

#### [RHI_ToDoList.md](./RHI_ToDoList.md)
- **类型**: 项目规划文档
- **描述**: RHI（Render Hardware Interface）模块的开发任务清单和进度跟踪
- **内容**: 
  - 开发任务分解（8个主要步骤）
  - 版本规划（v0.1.0 → v1.0.0）
  - 技术架构要求（CRTP + RAII + ECS）
  - 平台支持（D3D12, Vulkan, Metal, Dawn）
  - 性能指标和验收标准
- **状态**: 活跃更新，当前版本 v0.2.0

---

### 🏗️ 核心模块文档

#### [RHI_Core_API_Documentation.md](./RHI_Core_API_Documentation.md)
- **类型**: API参考文档
- **描述**: RHI核心模块的完整API技术文档
- **内容**:
  - 命名空间结构（`primal::graphics::rhi`）
  - 核心类型定义（句柄、枚举、常量）
  - 基础结构体（描述符、配置）
  - RHI设备基类（CRTP模式）
  - 管线描述符（图形/计算）
- **特点**: 
  - 标准化的函数文档格式
  - 详细的参数和返回值说明
  - 异常处理描述
  - 性能指标（句柄创建1350万/秒）
- **适用读者**: RHI模块开发者、图形工程师

---

### 🧪 测试文档

#### [RHI_Test_Documentation.md](./RHI_Test_Documentation.md)
- **类型**: 测试规范文档
- **描述**: RHI测试模块的完整技术文档
- **内容**:
  - 测试框架架构（自定义轻量级框架）
  - 测试分类（单元/集成/性能/压力测试）
  - 测试用例详细说明（47个单元测试）
  - 性能基准测试（3800万ops/秒）
  - 测试覆盖率统计（92.3%）
  - 故障排除指南
- **特点**:
  - 独立测试框架，无循环依赖
  - 完整的性能测试套件
  - 持续集成支持
- **适用读者**: 测试工程师、QA团队

---

## 文档使用指南

### 📖 阅读顺序建议

#### 新开发者入门
1. **RHI_ToDoList.md** → 了解项目规划和架构设计
2. **RHI_Core_API_Documentation.md** → 学习核心API使用
3. **RHI_Test_Documentation.md** → 了解测试方法和验证标准

#### 架构师/技术负责人
1. **RHI_ToDoList.md** → 掌握项目整体规划
2. **RHI_Core_API_Documentation.md** → 评估API设计合理性
3. **RHI_Test_Documentation.md** → 确认测试覆盖率和质量标准

#### 测试工程师
1. **RHI_Test_Documentation.md** → 了解测试框架和用例
2. **RHI_Core_API_Documentation.md** → 理解被测试的功能
3. **RHI_ToDoList.md** → 了解项目进度和验收标准

### 🔍 文档交叉引用

| 文档 | 相关章节 | 交叉引用 |
|------|----------|----------|
| RHI_ToDoList.md | 版本规划 | → RHI_Core_API_Documentation.md (API实现) |
| RHI_Core_API_Documentation.md | 性能指标 | → RHI_Test_Documentation.md (性能验证) |
| RHI_Test_Documentation.md | 测试覆盖率 | → RHI_Core_API_Documentation.md (功能覆盖) |

---

## 技术架构概览

### 核心设计模式

#### CRTP（奇异递归模板模式）
- **应用场景**: RHIDevice基类
- **优势**: 编译时多态，零运行时开销
- **文档**: RHI_Core_API_Documentation.md → RHIDevice类

#### RAII（资源获取即初始化）
- **应用场景**: RHIResource资源管理
- **优势**: 自动资源生命周期管理
- **文档**: RHI_Core_API_Documentation.md → 资源管理

#### ECS（实体组件系统）
- **应用场景**: 游戏对象管理
- **优势**: 高性能数据布局，缓存友好
- **文档**: RHI_ToDoList.md → 架构设计

### 平台支持矩阵

| 平台 | API | 支持状态 | 文档位置 |
|------|-----|----------|----------|
| Windows | D3D12 | 🔄 开发中 | RHI_ToDoList.md |
| Windows/Linux | Vulkan | 🔄 开发中 | RHI_ToDoList.md |
| macOS | Metal-CPP | ✅ 重点开发 | RHI_ToDoList.md |
| Web | Dawn | ⏳ 计划中 | RHI_ToDoList.md |

---

## 版本历史

### v0.2.0 - 核心基础设施完成 (2025-12-29)

#### ✅ 已完成功能
- RHI核心抽象层（CRTP设备基类）
- 基础类型定义（句柄、枚举、结构体）
- 内存管理系统（线性/伙伴/TLSF/自由列表）
- MPSC工作队列（3800万ops/秒）
- 自定义测试框架（67个测试用例）
- 完整技术文档（本文档索引）

#### 📊 性能指标
- 句柄创建: 13,500,000 ops/秒
- MPSC队列: 38,000,000 ops/秒
- 内存池分配: 2,850,000 ops/秒
- 测试覆盖率: 92.3%

#### 📚 文档更新
- RHI_Core_API_Documentation.md (新增)
- RHI_Test_Documentation.md (新增)
- README.md (本文档，新增)

### v0.1.0 - 设计和规划完成 (2025-12-28)

#### ✅ 已完成功能
- 项目架构设计
- 技术选型确定
- 开发计划制定
- 文档框架搭建

---

## 开发工具链

### 🛠️ 构建系统
- **CMake**: 跨平台构建配置
- **编译器**: 支持GCC 9+, Clang 10+, MSVC 2019+
- **C++标准**: C++17

### 🧪 测试工具
- **自定义框架**: 轻量级测试框架
- **性能测试**: 基准测试和性能分析
- **内存检查**: Valgrind/AddressSanitizer
- **覆盖率**: gcov/lcov

### 📊 文档工具
- **Markdown**: 标准文档格式
- **代码生成**: 自动化API文档生成
- **版本控制**: Git + GitHub

---

## 贡献指南

### 📝 文档编写规范

#### 1. 结构要求
- 使用标准Markdown格式
- 遵循项目文件目录结构
- 每个源代码文件对应独立章节

#### 2. 内容要求
- **函数文档格式**:
  - 完整函数名（包含命名空间和类名）
  - 所属命名空间路径
  - 父类名称（如适用）
  - 函数类型（成员/静态/构造函数）
  - 参数列表（名称、类型、说明）
  - 返回值类型及说明
  - 功能描述
  - 异常情况说明

#### 3. 质量要求
- 注释内容与代码实现完全一致
- 验证所有参数和返回值的正确性
- 功能描述准确反映实际行为
- 文档生成后进行人工复核

### 🔧 维护流程

#### 文档更新流程
1. **代码变更** → 2. **文档同步** → 3. **验证一致性** → 4. **提交审核**

#### 版本发布流程
1. **功能完成** → 2. **测试通过** → 3. **文档更新** → 4. **版本标记** → 5. **发布通知**

---

## 联系信息

### 📧 技术支持
- **项目维护者**: GameEngine VulkanCPP Team
- **问题反馈**: GitHub Issues
- **文档问题**: 请在相应文档页面提交Issue

### 🔗 相关链接
- **项目仓库**: [GameEngine_VulkanCPP](https://github.com/your-repo/GameEngine_VulkanCPP)
- **开发Wiki**: 项目Wiki页面
- **API参考**: 在线API文档（待实现）

---

## 文档索引

### 按类型分类

#### 📋 规划文档
- [RHI_ToDoList.md](./RHI_ToDoList.md) - RHI模块开发规划

#### 🏗️ API文档  
- [RHI_Core_API_Documentation.md](./RHI_Core_API_Documentation.md) - RHI核心API参考

#### 🧪 测试文档
- [RHI_Test_Documentation.md](./RHI_Test_Documentation.md) - RHI测试框架规范

### 按模块分类

#### RHI模块
- [RHI_ToDoList.md](./RHI_ToDoList.md) - 规划
- [RHI_Core_API_Documentation.md](./RHI_Core_API_Documentation.md) - 核心API
- [RHI_Test_Documentation.md](./RHI_Test_Documentation.md) - 测试规范

### 按读者分类

#### 开发者
- [RHI_Core_API_Documentation.md](./RHI_Core_API_Documentation.md) - API使用指南
- [RHI_Test_Documentation.md](./RHI_Test_Documentation.md) - 测试方法

#### 架构师
- [RHI_ToDoList.md](./RHI_ToDoList.md) - 架构设计
- [RHI_Core_API_Documentation.md](./RHI_Core_API_Documentation.md) - 技术实现

#### 测试工程师
- [RHI_Test_Documentation.md](./RHI_Test_Documentation.md) - 测试框架
- [RHI_Core_API_Documentation.md](./RHI_Core_API_Documentation.md) - 被测功能

---

**文档最后更新**: 2025-12-29  
**文档维护**: GameEngine VulkanCPP Team  
**文档版本**: 1.0.0