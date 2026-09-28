import QtQuick

/**
 * 属性绑定优化器
 * 提供缓存和延迟计算功能，减少不必要的属性重新计算
 */
QtObject {
    id: bindingOptimizer
    
    /**
     * 响应式列数计算（带缓存）
     * 根据宽度计算网格列数，避免频繁重新计算
     */
    function calculateColumns(width, breakpoints) {
        // breakpoints 格式: [{width: 700, columns: 2}, {width: 1000, columns: 3}, ...]
        // 必须按width从小到大排序
        
        if (!breakpoints || breakpoints.length === 0) {
            return 1
        }
        
        for (var i = breakpoints.length - 1; i >= 0; i--) {
            if (width >= breakpoints[i].width) {
                return breakpoints[i].columns
            }
        }
        
        // 如果小于最小断点，返回第一个断点的列数
        return breakpoints[0].columns
    }
    
    /**
     * 防抖函数 - 延迟执行回调，避免高频触发
     * @param callback 要执行的函数
     * @param delay 延迟时间（毫秒）
     * @return Timer对象
     */
    function debounce(callback, delay) {
        return Qt.createQmlObject(
            'import QtQuick; Timer { interval: ' + delay + '; repeat: false; onTriggered: (' + callback.toString() + ')() }',
            bindingOptimizer
        )
    }
    
    /**
     * 节流函数 - 限制函数执行频率
     * @param callback 要执行的函数
     * @param interval 最小间隔（毫秒）
     * @return 节流后的函数对象
     */
    function throttle(callback, interval) {
        return {
            lastTime: 0,
            timer: null,
            execute: function() {
                var now = Date.now()
                if (now - this.lastTime >= interval) {
                    callback()
                    this.lastTime = now
                }
            }
        }
    }
    
    /**
     * 颜色状态计算器
     * 根据组件状态（down/hovered/normal）返回对应颜色
     */
    function stateColor(down, hovered, downColor, hoverColor, normalColor) {
        if (down) return downColor
        if (hovered) return hoverColor
        return normalColor
    }
    
    /**
     * 条件级联计算器
     * 替代多层嵌套的三元表达式
     * @param value 要判断的值
     * @param conditions 条件数组 [{condition: func, result: value}, ...]
     * @param defaultResult 默认结果
     */
    function cascade(value, conditions, defaultResult) {
        for (var i = 0; i < conditions.length; i++) {
            if (conditions[i].condition(value)) {
                return conditions[i].result
            }
        }
        return defaultResult
    }
    
    /**
     * 范围映射
     * 将值从一个范围映射到另一个范围
     */
    function mapRange(value, inMin, inMax, outMin, outMax) {
        return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin
    }
    
    /**
     * 计算自适应字体大小
     */
    function responsiveFontSize(baseSize, width, minWidth, maxWidth) {
        if (width <= minWidth) return baseSize * 0.8
        if (width >= maxWidth) return baseSize * 1.2
        return mapRange(width, minWidth, maxWidth, baseSize * 0.8, baseSize * 1.2)
    }
    
    /**
     * 网格布局计算器
     * 计算网格单元格的尺寸
     */
    function gridCellSize(containerWidth, columns, spacing) {
        var totalSpacing = spacing * (columns - 1)
        var availableWidth = containerWidth - totalSpacing
        return availableWidth / columns
    }
    
    /**
     * 缓存计算结果
     * 使用LRU缓存避免重复计算
     */
    property var computeCache: ({})
    property var cacheLRU: []
    property int maxCacheSize: 100
    
    function cachedCompute(key, computeFunc) {
        // 检查缓存
        if (computeCache[key] !== undefined) {
            // 更新LRU
            updateLRU(key)
            return computeCache[key]
        }
        
        // 计算结果
        var result = computeFunc()
        
        // 检查缓存大小
        if (cacheLRU.length >= maxCacheSize) {
            // 移除最旧的缓存项
            var oldest = cacheLRU.shift()
            delete computeCache[oldest]
        }
        
        // 添加到缓存
        computeCache[key] = result
        cacheLRU.push(key)
        
        return result
    }
    
    function updateLRU(key) {
        var index = cacheLRU.indexOf(key)
        if (index > -1) {
            cacheLRU.splice(index, 1)
            cacheLRU.push(key)
        }
    }
    
    function clearCache() {
        computeCache = {}
        cacheLRU = []
    }
}
