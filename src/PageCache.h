#pragma once

#include "Common.h"
#include "ObjectPool.h"
#include "PageMap.h"

class PageCache
{
public:
    static PageCache *GetInstance()
    {
        return &_sInst;
    }

    // 获取从对象到span的映射
    Span *MapObjectToSpan(void *obj);

    // 释放空闲span回到Pagecache，并合并相邻的span
    void ReleaseSpanToPageCache(Span *span);

    // 获取一个K页的span
    Span *NewSpan(size_t k);

    std::mutex _pageMtx;

private:
    SpanList _spanLists[NPAGES];
    ObjectPool<Span> _spanPool;

    // std::unordered_map<PAGE_ID, Span*> _idSpanMap;
    // std::map<PAGE_ID, Span*> _idSpanMap;

    // 页号 -> Span 的映射表，用三级基数树实现：中间节点和叶子节点都按需分配，
    // PAGE_MAP_BITS 覆盖整个进程地址空间（x64 为 34 位页号 = 128TB，Win32 为 19 位 = 4GB）。
    // （原来的单层数组 TCMalloc_PageMap1<32 - PAGE_SHIFT> 只有 2^19 个槽位、只能覆盖 4GB，
    //   而现代 Windows 的 VirtualAlloc 会返回 4GB 以上的地址，set 越界写会直接崩溃）
    TCMalloc_PageMap3<PAGE_MAP_BITS> _idSpanMap;

    PageCache()
        : _idSpanMap(&SystemAllocBytes)
    {
    }
    PageCache(const PageCache &) = delete;

    static PageCache _sInst;
};
