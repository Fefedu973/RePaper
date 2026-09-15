#pragma once
#include <QAbstractListModel>
#include <QColor>

// Deterministic double for the documented firmware model roles. In particular
// green uses ARGB code 9, so the UI cannot infer it from a legacy enum value.
class MockNativePenColorModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(int colorProfile READ colorProfile WRITE setColorProfile NOTIFY colorProfileChanged)
    struct Color { const char *name; int palette; quint32 rgb; };
    const QVector<Color> colors{{"Black",0,0xff000000},{"Red",9,0xffd90707},
        {"Green",9,0xff249b45},{"Blue",9,0xff0062cc}};
    int m_tool=0,m_profile=0;
public:
    using QAbstractListModel::QAbstractListModel;
    enum Roles { Name=Qt::UserRole+1, DisplayColor, ToolColor, Rgb };
    int tool() const{return m_tool;}
    int colorProfile() const{return m_profile;}
    void setTool(int value){if(m_tool==value)return;beginResetModel();m_tool=value;endResetModel();emit toolChanged();}
    void setColorProfile(int value){if(m_profile==value)return;beginResetModel();m_profile=value;endResetModel();emit colorProfileChanged();}
    int rowCount(const QModelIndex &parent={}) const override{return parent.isValid()?0:m_tool==99?1:colors.size();}
    QHash<int,QByteArray> roleNames() const override{return {{Name,"displayName"},{DisplayColor,"displayColor"},{ToolColor,"toolColor"},{Rgb,"rgb"}};}
    QVariant data(const QModelIndex &index,int role) const override {
        if(!index.isValid()||index.row()<0||index.row()>=rowCount())return {};
        const auto &color=colors.at(index.row());
        switch(role){case Name:return QString::fromLatin1(color.name);case DisplayColor:return QColor::fromRgba(color.rgb);
        case ToolColor:return color.palette;case Rgb:return color.rgb;default:return {};}
    }
    Q_INVOKABLE bool isValidColor(int palette,quint32 rgb) const {
        for(int i=0;i<rowCount();++i)if(colors[i].palette==palette&&(palette!=9||colors[i].rgb==rgb))return true;
        return false;
    }
    Q_INVOKABLE QColor displayColor(int palette,quint32 rgb) const {
        if(palette==9)return QColor::fromRgba(rgb);
        for(const auto &color:colors)if(color.palette==palette)return QColor::fromRgba(color.rgb);
        return {};
    }
signals:
    void toolChanged();
    void colorProfileChanged();
};
