import QtQuick

/**
 * 图片预加载器
 * 用于后台预加载图片到缓存
 */
Item {
    id: preloader
    
    property url imageUrl
    property var cacheManager
    
    Image {
        id: preloadImage
        source: imageUrl
        asynchronous: true
        cache: true
        visible: false
        
        onStatusChanged: {
            if (status === Image.Ready) {
                // 加载完成，添加到缓存
                if (cacheManager) {
                    cacheManager.addToCache(imageUrl, preloadImage)
                }
                // 销毁预加载器
                preloader.destroy()
            } else if (status === Image.Error) {
                // 加载失败，直接销毁
                preloader.destroy()
            }
        }
    }
}
