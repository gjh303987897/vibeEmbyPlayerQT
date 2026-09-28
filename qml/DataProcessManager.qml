import QtQuick
import QtQml.WorkerScript

/**
 * 数据处理管理器
 * 使用Worker线程处理数据过滤、排序和搜索，避免阻塞UI线程
 */
QtObject {
    id: dataManager
    
    // Worker实例
    property WorkerScript worker: WorkerScript {
        source: "DataWorker.mjs"
        
        onMessage: function(message) {
            if (!message.success) {
                console.error("Worker错误:", message.error)
                return
            }
            
            // 触发相应的完成信号
            switch(message.action) {
                case 'filter':
                    dataManager.filterCompleted(message.items, message.count)
                    break
                case 'sort':
                    dataManager.sortCompleted(message.items, message.count)
                    break
                case 'filterAndSort':
                    dataManager.filterAndSortCompleted(message.items, message.count)
                    break
                case 'search':
                    dataManager.searchCompleted(message.items, message.count)
                    break
            }
        }
    }
    
    // 完成信号
    signal filterCompleted(var items, int count)
    signal sortCompleted(var items, int count)
    signal filterAndSortCompleted(var items, int count)
    signal searchCompleted(var items, int count)
    
    /**
     * 过滤数据
     * @param items 数据数组
     * @param filterFunc 过滤函数类型: 'type', 'status', 'contains', 'extension', 'date', 'custom'
     * @param filterValue 过滤值
     */
    function filter(items, filterFunc, filterValue) {
        worker.sendMessage({
            action: 'filter',
            data: {
                items: items,
                filterFunc: filterFunc,
                filterValue: filterValue
            }
        })
    }
    
    /**
     * 排序数据
     * @param items 数据数组
     * @param sortKey 排序键（支持嵌套：'parent.child.key'）
     * @param ascending 是否升序
     */
    function sort(items, sortKey, ascending) {
        if (ascending === undefined) {
            ascending = true
        }
        
        worker.sendMessage({
            action: 'sort',
            data: {
                items: items,
                sortKey: sortKey,
                ascending: ascending
            }
        })
    }
    
    /**
     * 先过滤后排序
     * @param items 数据数组
     * @param filterFunc 过滤函数类型
     * @param filterValue 过滤值
     * @param sortKey 排序键
     * @param ascending 是否升序
     */
    function filterAndSort(items, filterFunc, filterValue, sortKey, ascending) {
        if (ascending === undefined) {
            ascending = true
        }
        
        worker.sendMessage({
            action: 'filterAndSort',
            data: {
                items: items,
                filterFunc: filterFunc,
                filterValue: filterValue,
                sortKey: sortKey,
                ascending: ascending
            }
        })
    }
    
    /**
     * 搜索数据
     * @param items 数据数组
     * @param searchText 搜索文本
     * @param searchFields 搜索字段数组（可选，不指定则搜索所有字符串字段）
     */
    function search(items, searchText, searchFields) {
        worker.sendMessage({
            action: 'search',
            data: {
                items: items,
                searchText: searchText,
                searchFields: searchFields || []
            }
        })
    }
}
