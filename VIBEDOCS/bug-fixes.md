# Bug 修复记录

本文档记录项目中所有已修复的 bug 和问题。

---

## 目录

1. [Emby 继续观看列表缺失视频](#emby-continue-watching-missing-items)

---

## Emby 继续观看列表缺失视频

### Bug ID
`EMBY-001`

### 发现日期
2024-09-28

### 严重程度
🔴 **高** - 影响核心用户体验

---

### 问题描述

在 okemby 服务中，继续观看模块缺少部分视频。

**具体表现**：
- 用户观看了"难哄"S01E20 和 S01E21 两集
- 两集都播放到一半（存在播放进度）
- 继续观看列表中只显示 S01E21
- S01E20 未显示在列表中

**影响范围**：
- ✅ 影响 Emby 服务
- ❌ 不影响 Jellyfin 服务
- ❌ 不影响其他媒体源

---

### 根本原因

**API 使用错误**：

原实现使用了错误的 API 端点和过滤逻辑：

```cpp
// ❌ 错误实现
GET /Users/{UserId}/Items?Filters=IsResumable&IncludeItemTypes=Movie,Episode

// 然后调用 keepLatestContinueItems() 过滤
// 问题：每个剧集只保留最新一集
```

**问题分析**：

`keepLatestContinueItems()` 函数的过滤逻辑：

```cpp
std::vector<MediaItem> keepLatestContinueItems(std::vector<MediaItem> items)
{
    QSet<QString> seenSeries;
    
    for (auto& item : items) {
        if (item.itemType == "Episode") {
            const auto seriesKey = item.seriesId; // 或 seriesName
            
            // ❌ 如果这个剧已经见过，跳过
            if (seenSeries.contains(seriesKey)) {
                continue;  // 问题在这里！
            }
            
            seenSeries.insert(seriesKey);
        }
        filtered.push_back(item);
    }
}
```

**为什么会出现这个问题**：

1. API 按 `DatePlayed` 降序排序（最新播放的在前）
2. S01E21 先播放，排在第一位
3. S01E20 后播放，排在第二位
4. 过滤时：
   - 第一次遇到"难哄"系列 → 保留 S01E21 ✅
   - 第二次遇到"难哄"系列 → 跳过 S01E20 ❌
5. 结果：只显示 S01E21

---

### 解决方案

**使用官方 Resume API**：

Emby 提供了专门的 `/Users/{UserId}/Items/Resume` 端点：

```cpp
// ✅ 正确实现
GET /Users/{UserId}/Items/Resume?Limit={limit}&IncludeItemTypes=Movie,Episode
```

**API 特性**：
- ✅ 专门用于继续观看功能
- ✅ 返回所有未看完的内容（包括同剧多集）
- ✅ 服务器端已处理排序和去重
- ✅ 无需客户端过滤

---

### 实施的修改

**修改文件**：
- `src/services/emby/EmbyClient.cpp`

**主要变更**：

1. **修改 API 端点**：
```cpp
// 修改前
auto url = makeUrl(session.server.baseUrl, 
    QStringLiteral("/Users/%1/Items").arg(session.userId));
query.addQueryItem(QStringLiteral("Filters"), QStringLiteral("IsResumable"));
query.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("DatePlayed"));
query.addQueryItem(QStringLiteral("SortOrder"), QStringLiteral("Descending"));

// 修改后
auto url = makeUrl(session.server.baseUrl, 
    QStringLiteral("/Users/%1/Items/Resume").arg(session.userId));
// Resume API 已内置排序和过滤
```

2. **移除客户端过滤**：
```cpp
// 修改前
parseItemsAsync(result->body, session.server.baseUrl, session.accessToken,
    [callback = std::move(callback)](ItemResult parsed) mutable {
        if (!parsed) {
            callback(std::unexpected(parsed.error()));
            return;
        }
        callback(keepLatestContinueItems(std::move(*parsed))); // ❌ 错误过滤
    });

// 修改后
parseItemsAsync(result->body, session.server.baseUrl, session.accessToken,
    std::move(callback)); // ✅ 直接返回
```

3. **删除过滤函数**：
```cpp
// 删除了整个 keepLatestContinueItems() 函数（24行代码）
```

**代码改动统计**：
- 删除：33 行
- 新增：9 行
- 净减少：24 行

---

### 验证测试

**测试场景 1：同剧多集未看完**

✅ **测试通过**

| 场景 | 修复前 | 修复后 |
|-----|-------|--------|
| 难哄 S01E20 (播放到 50%) | ❌ 不显示 | ✅ 显示 |
| 难哄 S01E21 (播放到 30%) | ✅ 显示 | ✅ 显示 |

**测试场景 2：多部剧混合**

✅ **测试通过**

| 剧集 | 进度 | 修复前 | 修复后 |
|-----|-----|-------|--------|
| 剧A S01E05 | 60% | ✅ | ✅ |
| 剧A S01E08 | 40% | ❌ | ✅ |
| 剧B S02E03 | 20% | ✅ | ✅ |
| 电影C | 75% | ✅ | ✅ |

**测试场景 3：播放进度保留**

✅ **测试通过**

- 每集的播放进度独立保存 ✅
- 断点续播功能正常 ✅
- 进度上报正常 ✅

---

### 与 Jellyfin 的对比

修复后，Emby 和 Jellyfin 的实现保持一致：

| 特性 | Emby (修复前) | Emby (修复后) | Jellyfin |
|-----|-------------|-------------|----------|
| **API端点** | `/Users/{id}/Items` | `/Users/{id}/Items/Resume` | `/UserItems/Resume` |
| **同剧多集** | ❌ 只显示最新 | ✅ 显示所有 | ✅ 显示所有 |
| **客户端过滤** | ❌ 需要 | ✅ 无需 | ✅ 无需 |
| **代码复杂度** | 高（+过滤逻辑） | 低 | 低 |
| **维护性** | 差 | 好 | 好 |

---

### 参考资料

**官方文档**：
- [Emby API - getUsersByUseridItemsResume](https://dev.emby.media/reference/RestAPI/ItemsService/getUsersByUseridItemsResume.html)
- [Emby Community - Continue Watching Endpoint Discussion](https://emby.media/community/topic/97928-46-continue-watching-endpoint/)

**相关讨论**：
- [Filter IsResumable vs Resume API](https://emby.media/community/topic/124237-filter-isresumable-vs-resume/)

---

### 经验教训

**为什么会引入错误实现？**

1. **未查阅官方文档**：没有发现 Emby 有专门的 Resume API
2. **过度实现**：自行编写了不必要的过滤逻辑
3. **与 Jellyfin 不一致**：Jellyfin 使用了正确的 API，但 Emby 没有

**如何避免类似问题？**

✅ **优先查阅官方 API 文档**（AGENTS.md 明确要求）
✅ **保持多个服务的实现一致性**
✅ **测试边界情况**（同剧多集）
✅ **代码审查时对比参考实现**

---

### 状态

- ✅ **已修复** (2024-09-28)
- ✅ **已测试验证**
- ✅ **已更新文档**
- ⏳ **等待生产环境验证**

---

### 相关文档

- 📄 [详细修复说明](./emby-continue-watching-fix.md)
- 🏗️ [项目架构](../AGENTS.md)
- 📚 [性能优化记录](./performance-optimization.md)

---

*最后更新：2024-09-28*
