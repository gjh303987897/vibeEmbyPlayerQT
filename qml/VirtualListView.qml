import QtQuick
import QtQuick.Controls

/**
 * 虚拟化列表视图
 * 只渲染可见区域的item，大幅提升大列表性能
 */
Item {
    id: virtualList
    
    // 数据模型
    property var model: []
    
    // Delegate组件
    property Component delegate
    
    // Item高度（固定高度模式）
    property real itemHeight: 60
    
    // Item宽度
    property real itemWidth: width
    
    // 垂直间距
    property real spacing: 0
    
    // 缓冲区大小（屏幕外预渲染的item数量）
    property int bufferSize: 3
    
    // 方向：垂直或水平
    property int orientation: ListView.Vertical
    
    // 滚动位置
    property real contentY: flickable.contentY
    property real contentX: flickable.contentX
    
    // 当前可见范围
    readonly property int firstVisibleIndex: {
        if (orientation === ListView.Vertical) {
            return Math.max(0, Math.floor(flickable.contentY / (itemHeight + spacing)) - bufferSize)
        } else {
            return Math.max(0, Math.floor(flickable.contentX / (itemWidth + spacing)) - bufferSize)
        }
    }
    
    readonly property int lastVisibleIndex: {
        if (orientation === ListView.Vertical) {
            var visibleCount = Math.ceil(flickable.height / (itemHeight + spacing)) + bufferSize * 2
            return Math.min(model.length - 1, firstVisibleIndex + visibleCount)
        } else {
            var visibleCount = Math.ceil(flickable.width / (itemWidth + spacing)) + bufferSize * 2
            return Math.min(model.length - 1, firstVisibleIndex + visibleCount)
        }
    }
    
    // 总内容大小
    readonly property real contentHeight: orientation === ListView.Vertical 
        ? model.length * (itemHeight + spacing) - spacing 
        : flickable.height
    
    readonly property real contentWidth: orientation === ListView.Horizontal 
        ? model.length * (itemWidth + spacing) - spacing 
        : flickable.width
    
    // 当前激活的delegates池
    property var activeDelegates: ({})
    
    // delegate池（用于复用）
    property var delegatePool: []
    
    // 最大池大小
    property int maxPoolSize: 20
    
    Flickable {
        id: flickable
        anchors.fill: parent
        
        contentWidth: virtualList.contentWidth
        contentHeight: virtualList.contentHeight
        
        clip: true
        
        // 平滑滚动
        flickDeceleration: 1500
        maximumFlickVelocity: 2500
        
        // 滚动条
        ScrollBar.vertical: ScrollBar {
            visible: orientation === ListView.Vertical
            policy: ScrollBar.AsNeeded
        }
        
        ScrollBar.horizontal: ScrollBar {
            visible: orientation === ListView.Horizontal
            policy: ScrollBar.AsNeeded
        }
        
        // 容器
        Item {
            id: contentItem
            width: flickable.contentWidth
            height: flickable.contentHeight
        }
    }
    
    // 监听可见范围变化，更新delegates
    onFirstVisibleIndexChanged: updateVisibleDelegates()
    onLastVisibleIndexChanged: updateVisibleDelegates()
    onModelChanged: {
        // 模型变化时，清空所有delegates
        clearAllDelegates()
        updateVisibleDelegates()
    }
    
    /**
     * 更新可见区域的delegates
     */
    function updateVisibleDelegates() {
        if (!delegate || !model || model.length === 0) {
            return
        }
        
        var newActiveDelegates = {}
        
        // 创建或复用可见范围内的delegates
        for (var i = firstVisibleIndex; i <= lastVisibleIndex; i++) {
            if (i < 0 || i >= model.length) {
                continue
            }
            
            var delegateItem = activeDelegates[i]
            
            if (!delegateItem) {
                // 从池中获取或创建新的delegate
                delegateItem = getDelegateFromPool()
                if (!delegateItem) {
                    delegateItem = delegate.createObject(contentItem)
                }
                
                if (delegateItem) {
                    // 设置model数据
                    delegateItem.model = model[i]
                    delegateItem.index = i
                    
                    // 设置位置
                    if (orientation === ListView.Vertical) {
                        delegateItem.x = 0
                        delegateItem.y = i * (itemHeight + spacing)
                        delegateItem.width = Qt.binding(function() { return virtualList.width })
                        delegateItem.height = itemHeight
                    } else {
                        delegateItem.x = i * (itemWidth + spacing)
                        delegateItem.y = 0
                        delegateItem.width = itemWidth
                        delegateItem.height = Qt.binding(function() { return virtualList.height })
                    }
                    
                    delegateItem.visible = true
                }
            }
            
            newActiveDelegates[i] = delegateItem
        }
        
        // 回收不可见的delegates
        for (var index in activeDelegates) {
            if (!newActiveDelegates[index]) {
                returnDelegateToPool(activeDelegates[index])
            }
        }
        
        activeDelegates = newActiveDelegates
    }
    
    /**
     * 从池中获取delegate
     */
    function getDelegateFromPool() {
        if (delegatePool.length > 0) {
            return delegatePool.pop()
        }
        return null
    }
    
    /**
     * 将delegate返回到池中
     */
    function returnDelegateToPool(delegateItem) {
        if (!delegateItem) {
            return
        }
        
        if (delegatePool.length < maxPoolSize) {
            // 隐藏并移出屏幕外
            delegateItem.visible = false
            delegateItem.x = -9999
            delegateItem.y = -9999
            delegatePool.push(delegateItem)
        } else {
            // 池已满，销毁
            delegateItem.destroy()
        }
    }
    
    /**
     * 清空所有delegates
     */
    function clearAllDelegates() {
        // 回收所有激活的delegates
        for (var index in activeDelegates) {
            returnDelegateToPool(activeDelegates[index])
        }
        activeDelegates = {}
        
        // 清空池
        for (var i = 0; i < delegatePool.length; i++) {
            if (delegatePool[i]) {
                delegatePool[i].destroy()
            }
        }
        delegatePool = []
    }
    
    /**
     * 滚动到指定索引
     */
    function positionViewAtIndex(index, mode) {
        if (index < 0 || index >= model.length) {
            return
        }
        
        if (orientation === ListView.Vertical) {
            var targetY = index * (itemHeight + spacing)
            
            switch(mode) {
                case ListView.Beginning:
                    flickable.contentY = targetY
                    break
                case ListView.Center:
                    flickable.contentY = targetY - (flickable.height - itemHeight) / 2
                    break
                case ListView.End:
                    flickable.contentY = targetY - flickable.height + itemHeight
                    break
                default:
                    // 如果不在可见范围，滚动到该位置
                    if (targetY < flickable.contentY) {
                        flickable.contentY = targetY
                    } else if (targetY + itemHeight > flickable.contentY + flickable.height) {
                        flickable.contentY = targetY - flickable.height + itemHeight
                    }
            }
        } else {
            var targetX = index * (itemWidth + spacing)
            
            switch(mode) {
                case ListView.Beginning:
                    flickable.contentX = targetX
                    break
                case ListView.Center:
                    flickable.contentX = targetX - (flickable.width - itemWidth) / 2
                    break
                case ListView.End:
                    flickable.contentX = targetX - flickable.width + itemWidth
                    break
                default:
                    if (targetX < flickable.contentX) {
                        flickable.contentX = targetX
                    } else if (targetX + itemWidth > flickable.contentX + flickable.width) {
                        flickable.contentX = targetX - flickable.width + itemWidth
                    }
            }
        }
    }
    
    Component.onDestruction: {
        clearAllDelegates()
    }
}
