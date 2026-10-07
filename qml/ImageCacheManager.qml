import QtQuick
import QtQuick.Controls

/**
 * 图片缓存管理器
 * 提供缩略图缓存和预加载功能，提升列表滚动性能
 */
QtObject {
    id: cacheManager

    // 缓存大小限制（MB）
    property int maxCacheSizeMB: 100
    property int maxCacheEntries: 256
    property Component cachedImageComponent: Component {
        Image { asynchronous: true; cache: true; visible: false }
    }
    
    // 缓存的图片Map：url -> Image
    property var imageCache: ({})
    
    // 缓存大小计数（近似）
    property int currentCacheSizeBytes: 0
    
    // 预加载队列
    property var preloadQueue: []
    
    // 是否正在预加载
    property bool isPreloading: false
    
    // 缓存命中统计
    property int cacheHits: 0
    property int cacheMisses: 0
    
    /**
     * 获取缓存的图片
     * @param url 图片URL
     * @return 缓存的Image对象或null
     */
    function getCachedImage(url) {
        if (!url || url.toString().length === 0) {
            return null
        }
        
        var cached = imageCache[url.toString()]
        if (cached) {
            cacheHits++
            return cached
        }
        
        cacheMisses++
        return null
    }
    
    /**
     * 添加图片到缓存
     * @param url 图片URL
     * @param image Image组件实例
     */
    function addToCache(url, image) {
        if (!url || url.toString().length === 0) {
            return
        }
        
        var urlStr = url.toString()
        
        // 如果已经在缓存中，跳过
        if (imageCache[urlStr]) {
            return
        }
        
        // 估算图片大小（宽*高*4字节RGBA）
        if (!image) return
        var estimatedSize = image.sourceSize.width * image.sourceSize.height * 4
        var limit = Math.max(0, maxCacheSizeMB) * 1024 * 1024
        // Unknown dimensions cannot provide a bounded memory estimate.
        if (image.sourceSize.width <= 0 || image.sourceSize.height <= 0 ||
            estimatedSize > limit || maxCacheEntries <= 0) return
        
        // 如果缓存已满，清理旧缓存
        clearOldestCache(Math.max(0, currentCacheSizeBytes + estimatedSize - limit))
        // The preloader owns the input Image and destroys it after this call.
        // Keep a cache-owned Image; Qt shares its decoded pixmap for this URL.
        var retainedImage = cachedImageComponent.createObject(cacheManager, {
            source: urlStr, sourceSize: image.sourceSize
        })
        if (!retainedImage) return
        
        imageCache[urlStr] = {
            image: retainedImage,
            timestamp: Date.now(),
            size: estimatedSize
        }
        
        currentCacheSizeBytes += estimatedSize
    }
    
    /**
     * 清理最旧的缓存项
     * @param neededSize 需要的空间大小
     */
    function clearOldestCache(neededSize) {
        var entries = []
        for (var url in imageCache) {
            entries.push({
                url: url,
                timestamp: imageCache[url].timestamp,
                size: imageCache[url].size
            })
        }
        
        // 按时间排序，最旧的在前
        entries.sort(function(a, b) {
            return a.timestamp - b.timestamp
        })
        
        // 删除最旧的条目，直到有足够空间
        var freedSize = 0
        var remaining = entries.length
        for (var i = 0; i < entries.length && (freedSize < neededSize || remaining >= maxCacheEntries); i++) {
            var entry = entries[i]
            imageCache[entry.url].image.destroy()
            delete imageCache[entry.url]
            remaining--
            freedSize += entry.size
            currentCacheSizeBytes -= entry.size
        }
    }
    
    /**
     * 预加载图片列表
     * @param urls 图片URL数组
     */
    function preloadImages(urls) {
        if (!urls || urls.length === 0) {
            return
        }
        
        // 添加到预加载队列
        for (var i = 0; i < urls.length; i++) {
            var url = urls[i]
            if (url && url.toString().length > 0 && !getCachedImage(url)) {
                preloadQueue.push(url)
            }
        }
        
        // 开始预加载
        if (!isPreloading && preloadQueue.length > 0) {
            startPreload()
        }
    }
    
    /**
     * 开始预加载
     */
    function startPreload() {
        isPreloading = true
        preloadTimer.start()
    }
    
    /**
     * 清空所有缓存
     */
    function clearCache() {
        for (var url in imageCache) imageCache[url].image.destroy()
        imageCache = {}
        currentCacheSizeBytes = 0
        cacheHits = 0
        cacheMisses = 0
        console.log("图片缓存已清空")
    }
    
    /**
     * 获取缓存统计信息
     */
    function getCacheStats() {
        var cacheCount = 0
        for (var url in imageCache) {
            cacheCount++
        }
        
        return {
            count: cacheCount,
            sizeBytes: currentCacheSizeBytes,
            sizeMB: (currentCacheSizeBytes / (1024 * 1024)).toFixed(2),
            hits: cacheHits,
            misses: cacheMisses,
            hitRate: cacheMisses > 0 ? ((cacheHits / (cacheHits + cacheMisses)) * 100).toFixed(1) + "%" : "N/A"
        }
    }
    
    // 预加载定时器 - 每次加载一个图片，避免阻塞UI
    property Timer preloadTimer: Timer {
        interval: 50  // 50ms加载一个
        repeat: true
        onTriggered: {
            if (preloadQueue.length === 0) {
                stop()
                isPreloading = false
                return
            }
            
            var url = preloadQueue.shift()
            
            // 创建临时Image用于预加载
            var component = Qt.createComponent("ImagePreloader.qml")
            if (component.status === Component.Ready) {
                var preloader = component.createObject(cacheManager, {
                    imageUrl: url,
                    cacheManager: cacheManager
                })
            }
        }
    }
}
