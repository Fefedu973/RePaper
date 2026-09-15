#pragma once
#include <QVector>
#include <QtGlobal>
#include <algorithm>
#include <limits>
#include <utility>

namespace RePaperNative::ObjectAccessDetail {
struct MemoryRegion { quintptr first=0,last=0;bool write=false,execute=false; };

// /proc/self/maps is a disjoint address-ordered set. Keep that invariant and
// validate a range in logarithmic time, with O(1) repeated access to its current
// allocation. This index is local to one locked native operation, never shared
// across callbacks or cached over allocation changes.
class MemoryRanges {
public:
    explicit MemoryRanges(QVector<MemoryRegion> regions={}) : m_regions(std::move(regions)) {
        std::sort(m_regions.begin(),m_regions.end(),[](const auto &a,const auto &b){return a.first<b.first;});
        quintptr end=0;
        for(const auto &region:std::as_const(m_regions)){
            if(!region.first||region.first<end||region.last<=region.first){m_regions.clear();return;}
            end=region.last;
        }
    }
    bool contains(quintptr address,quintptr length,bool write=false,bool execute=false,
                  qsizetype *comparisons=nullptr) const {
        if(!address||!length||length>std::numeric_limits<quintptr>::max()-address)return false;
        const auto matches=[&](const MemoryRegion &region){
            return address>=region.first&&address+length<=region.last
                &&(!write||region.write)&&(!execute||region.execute);
        };
        if(m_last>=0){
            if(comparisons)++*comparisons;
            if(matches(m_regions[m_last]))return true;
        }
        const auto it=std::upper_bound(m_regions.cbegin(),m_regions.cend(),address,
            [&](quintptr value,const MemoryRegion &region){if(comparisons)++*comparisons;return value<region.first;});
        if(it==m_regions.cbegin())return false;
        m_last=(it-m_regions.cbegin())-1;
        return matches(m_regions[m_last]);
    }
private:
    QVector<MemoryRegion> m_regions;
    mutable qsizetype m_last=-1;
};
}
