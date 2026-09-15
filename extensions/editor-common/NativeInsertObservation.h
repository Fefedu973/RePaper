#pragma once
#include "NativeCreationSession.h"
#include <QSet>
#include <QRegularExpression>
#include <QCryptographicHash>

namespace RePaperNative {
// This recognizes a changed selection after dispatch only. Disjoint IDs and
// count cannot uniquely attribute it to our command (another native lasso can
// select other old items). It does not certify content, disk storage, Undo/Redo,
// ownership of semantic objects or durable identity reconciliation.
class NativeInsertObservation {
public:
    void begin(const QVariantMap &context,const QVariantMap &before,int expected) {
        clear();m_scope.arm(context,true);m_expected=expected;m_pending=true;
        m_beforeValid=ids(context,before,&m_before);
    }
    bool observe(const QVariantMap &context,const QVariantMap &after) {
        QSet<QString> next;
        if(!m_pending||!m_scope.matches(context)||!m_beforeValid||m_expected<1||m_expected>128
            ||!ids(context,after,&next)||next.size()!=m_expected)return false;
        for(const auto &id:next)if(m_before.contains(id))return false;
        m_pending=false;return true;
    }
    bool pending()const{return m_pending;}
    void clear(){m_pending=false;m_beforeValid=false;m_before.clear();m_expected=0;m_scope.clear();}
private:
    static bool ids(const QVariantMap &context,const QVariantMap &value,QSet<QString> *result){
        if(value.value("status")!="observed"||!value.value("complete").toBool())return false;
        const auto pageHash=QString::fromLatin1(QCryptographicHash::hash(context.value("pageId").toString().toUtf8(),QCryptographicHash::Sha256).toHex());
        if(value.value("pageIdSha256").toString()!=pageHash||!value.contains("layer")||value.value("layer")!=context.value("layer"))return false;
        const auto items=value.value("items").toList();
        // A new blank page has an empty valid baseline. Creation cannot depend
        // on a pre-existing lasso selection; only the AFTER count must be >0.
        if(items.size()>128||value.value("count").toInt()!=items.size())return false;
        static const QRegularExpression hex("^[0-9a-f]{16}$");
        for(const auto &entry:items){const auto item=entry.toMap();const auto id=item.value("idHex").toString().toLower();
            if(item.value("type")!="SceneLineItem"||!hex.match(id).hasMatch()||id=="0000000000000000"||result->contains(id))return false;
            result->insert(id);
        }
        return true;
    }
    NativeCreationSession m_scope;
    QSet<QString> m_before;
    bool m_pending=false,m_beforeValid=false;
    int m_expected=0;
};
}
