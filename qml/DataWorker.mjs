// DataWorker.mjs - Worker线程用于处理数据过滤和排序
// 避免在UI线程执行复杂数据操作

WorkerScript.onMessage = function(message) {
    var action = message.action
    var data = message.data
    var result = {}
    
    try {
        switch(action) {
            case 'filter':
                result = filterData(data.items, data.filterFunc, data.filterValue)
                break
            case 'sort':
                result = sortData(data.items, data.sortKey, data.ascending)
                break
            case 'filterAndSort':
                var filtered = filterData(data.items, data.filterFunc, data.filterValue)
                result = sortData(filtered.items, data.sortKey, data.ascending)
                break
            case 'search':
                result = searchData(data.items, data.searchText, data.searchFields)
                break
            default:
                result = { success: false, error: 'Unknown action: ' + action }
        }
        
        result.success = true
        result.action = action
    } catch(e) {
        result = {
            success: false,
            action: action,
            error: e.toString()
        }
    }
    
    WorkerScript.sendMessage(result)
}

/**
 * 过滤数据
 */
function filterData(items, filterFunc, filterValue) {
    if (!items || items.length === 0) {
        return { items: [], count: 0 }
    }
    
    // 如果没有过滤条件，返回全部
    if (!filterFunc || filterValue === undefined || filterValue === null || filterValue === '') {
        return { items: items, count: items.length }
    }
    
    var filtered = []
    
    for (var i = 0; i < items.length; i++) {
        var item = items[i]
        var pass = false
        
        // 根据filterFunc类型进行过滤
        switch(filterFunc) {
            case 'type':
                pass = item.type === filterValue
                break
            case 'status':
                pass = item.status === filterValue
                break
            case 'contains':
                pass = item.name && item.name.toLowerCase().indexOf(filterValue.toLowerCase()) >= 0
                break
            case 'extension':
                pass = item.name && item.name.toLowerCase().endsWith(filterValue.toLowerCase())
                break
            case 'date':
                // 日期过滤 - filterValue为日期范围对象 {start, end}
                if (filterValue.start && filterValue.end) {
                    var itemDate = item.dateModified || item.dateCreated || 0
                    pass = itemDate >= filterValue.start && itemDate <= filterValue.end
                }
                break
            case 'custom':
                // 自定义过滤 - filterValue是判断函数的字符串
                try {
                    var customFunc = eval('(' + filterValue + ')')
                    pass = customFunc(item)
                } catch(e) {
                    pass = false
                }
                break
            default:
                pass = true
        }
        
        if (pass) {
            filtered.push(item)
        }
    }
    
    return { items: filtered, count: filtered.length }
}

/**
 * 排序数据
 */
function sortData(items, sortKey, ascending) {
    if (!items || items.length === 0) {
        return { items: [], count: 0 }
    }
    
    // 如果没有排序键，返回原数组
    if (!sortKey) {
        return { items: items, count: items.length }
    }
    
    // 复制数组避免修改原数组
    var sorted = items.slice()
    
    sorted.sort(function(a, b) {
        var aVal = getNestedValue(a, sortKey)
        var bVal = getNestedValue(b, sortKey)
        
        // 处理undefined/null
        if (aVal === undefined || aVal === null) return 1
        if (bVal === undefined || bVal === null) return -1
        
        var comparison = 0
        
        // 根据类型比较
        if (typeof aVal === 'string' && typeof bVal === 'string') {
            comparison = aVal.localeCompare(bVal)
        } else if (typeof aVal === 'number' && typeof bVal === 'number') {
            comparison = aVal - bVal
        } else if (aVal instanceof Date && bVal instanceof Date) {
            comparison = aVal.getTime() - bVal.getTime()
        } else {
            // 转换为字符串比较
            comparison = String(aVal).localeCompare(String(bVal))
        }
        
        return ascending ? comparison : -comparison
    })
    
    return { items: sorted, count: sorted.length }
}

/**
 * 搜索数据
 */
function searchData(items, searchText, searchFields) {
    if (!items || items.length === 0) {
        return { items: [], count: 0 }
    }
    
    // 如果没有搜索文本，返回全部
    if (!searchText || searchText.trim().length === 0) {
        return { items: items, count: items.length }
    }
    
    var searchLower = searchText.toLowerCase().trim()
    var results = []
    
    for (var i = 0; i < items.length; i++) {
        var item = items[i]
        var found = false
        
        // 如果指定了搜索字段，只在这些字段中搜索
        if (searchFields && searchFields.length > 0) {
            for (var j = 0; j < searchFields.length; j++) {
                var fieldValue = getNestedValue(item, searchFields[j])
                if (fieldValue && String(fieldValue).toLowerCase().indexOf(searchLower) >= 0) {
                    found = true
                    break
                }
            }
        } else {
            // 否则在所有字符串字段中搜索
            for (var key in item) {
                var value = item[key]
                if (typeof value === 'string' && value.toLowerCase().indexOf(searchLower) >= 0) {
                    found = true
                    break
                }
            }
        }
        
        if (found) {
            results.push(item)
        }
    }
    
    return { items: results, count: results.length }
}

/**
 * 获取嵌套对象的值
 * 例如: getNestedValue({a: {b: {c: 1}}}, 'a.b.c') 返回 1
 */
function getNestedValue(obj, path) {
    if (!path) return obj
    
    var keys = path.split('.')
    var value = obj
    
    for (var i = 0; i < keys.length; i++) {
        if (value === undefined || value === null) {
            return undefined
        }
        value = value[keys[i]]
    }
    
    return value
}
