#include "ActivePageAdapter.h"
#include "InkCanvas.h"
#include <QCoreApplication>

ActivePageAdapter::ActivePageAdapter(QObject *parent):QObject(parent),
    m_emulator(QCoreApplication::arguments().contains("--emulator")) {}
bool ActivePageAdapter::available() const{return m_emulator&&!m_page.isNull();}
QString ActivePageAdapter::status() const {
    if(available())return "Page d’essai en processus : insertion, sélection et annulation disponibles. Validation Xochitl sur tablette non réalisée.";
    return "Insertion Xochitl désactivée. Aucun adaptateur d’édition natif n’a été validé sur Paper Pro 3.28.0.169.";
}
void ActivePageAdapter::attachEmulatorPage(QObject *page) {
    if(!m_emulator)return;
    m_page=qobject_cast<InkCanvas*>(page);emit changed();
}
bool ActivePageAdapter::insert(const QString &symbolId) {
    if(!available())return false;
    const int before=m_page->itemCount();m_page->insertSymbol(symbolId);
    return m_page->itemCount()==before+1;
}
