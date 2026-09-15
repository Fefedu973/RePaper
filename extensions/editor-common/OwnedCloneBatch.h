#pragma once
#include <QList>
#include <QSet>
#include <QString>
#include <memory>

namespace RePaperNative {
struct CloneBudget {
    qsizetype outputItems=128;
    qsizetype inputItemsPerCall=128;
    qsizetype inspectedItems=4096;
    int calls=128;
};

// Collect first, then let the caller alter each distinct owned clone. Fetch must
// return newly cloned objects, never live selected objects. This helper does not
// manufacture objects, dereference private fields or mutate supplied items.
template<class Item,class Fetch,class Accept>
QList<std::shared_ptr<Item>> collectOwnedClones(qsizetype count,Fetch fetch,Accept accept,
                                               QString *error,CloneBudget budget={}) {
    if(error)error->clear();
    const auto fail=[error](const char *message){if(error)*error=QString::fromUtf8(message);return QList<std::shared_ptr<Item>>{};};
    if(count<=0||count>budget.outputItems)return fail("Nombre de traits natifs hors limite.");
    QList<std::shared_ptr<Item>> result;
    QSet<const Item*> seen;
    qsizetype inspected=0;
    for(int attempt=0;result.size()<count&&attempt<budget.calls;++attempt) {
        bool valid=false;
        const auto batch=fetch(&valid);
        if(!valid)return fail("Contexte ou type natif de la sélection invalide.");
        if(batch.size()>budget.inputItemsPerCall||batch.size()>budget.inspectedItems-inspected)
            return fail("Sélection trop volumineuse pour préparer ce lot natif.");
        inspected+=batch.size();
        const qsizetype before=result.size();
        for(const auto &item:batch) {
            if(!item||!accept(item.get()))continue;
            if(seen.contains(item.get()))return fail("Xochitl a réutilisé un objet de clone ; insertion refusée.");
            seen.insert(item.get());
            result.append(item);
            if(result.size()==count)return result;
        }
        if(result.size()==before)return fail("Sélectionnez au moins un trait natif existant pour préparer ce dessin.");
    }
    return fail("Ce dessin demande trop de clonages natifs ; réduisez le lot ou sélectionnez davantage de traits.");
}
}
