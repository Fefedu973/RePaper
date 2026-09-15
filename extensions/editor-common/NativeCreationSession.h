#pragma once
#include <QVariantMap>

namespace RePaperNative {
// Explicit, volatile permission for one native test page/layer. This value has
// no settings/file serialization, and attaching a new page clears it.
class NativeCreationSession {
public:
    bool arm(const QVariantMap &context,bool ready) {
        clear();
        if(!ready||context.value("documentId").toString().isEmpty()||context.value("pageId").toString().isEmpty()
            ||!context.contains("layer")||context.value("layer").toInt()<0)return false;
        m_document=context.value("documentId").toString();m_page=context.value("pageId").toString();m_layer=context.value("layer").toInt();
        m_armed=true;return true;
    }
    bool matches(const QVariantMap &context) const {
        return m_armed&&context.value("documentId").toString()==m_document&&context.value("pageId").toString()==m_page
            &&context.contains("layer")&&context.value("layer").toInt()==m_layer;
    }
    bool armed() const {return m_armed;}
    void clear(){m_armed=false;m_document.clear();m_page.clear();m_layer=-1;}
private:
    bool m_armed=false;
    QString m_document,m_page;
    int m_layer=-1;
};
}
