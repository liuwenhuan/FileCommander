/****************************************************************************
** Meta object code from reading C++ file 'FunctionKeyBar.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../FunctionKeyBar.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'FunctionKeyBar.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_FunctionKeyBar_t {
    QByteArrayData data[8];
    char stringdata0[103];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_FunctionKeyBar_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_FunctionKeyBar_t qt_meta_stringdata_FunctionKeyBar = {
    {
QT_MOC_LITERAL(0, 0, 14), // "FunctionKeyBar"
QT_MOC_LITERAL(1, 15, 13), // "viewRequested"
QT_MOC_LITERAL(2, 29, 0), // ""
QT_MOC_LITERAL(3, 30, 13), // "editRequested"
QT_MOC_LITERAL(4, 44, 13), // "copyRequested"
QT_MOC_LITERAL(5, 58, 13), // "moveRequested"
QT_MOC_LITERAL(6, 72, 14), // "mkdirRequested"
QT_MOC_LITERAL(7, 87, 15) // "deleteRequested"

    },
    "FunctionKeyBar\0viewRequested\0\0"
    "editRequested\0copyRequested\0moveRequested\0"
    "mkdirRequested\0deleteRequested"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_FunctionKeyBar[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       6,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       6,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   44,    2, 0x06 /* Public */,
       3,    0,   45,    2, 0x06 /* Public */,
       4,    0,   46,    2, 0x06 /* Public */,
       5,    0,   47,    2, 0x06 /* Public */,
       6,    0,   48,    2, 0x06 /* Public */,
       7,    0,   49,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void FunctionKeyBar::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<FunctionKeyBar *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->viewRequested(); break;
        case 1: _t->editRequested(); break;
        case 2: _t->copyRequested(); break;
        case 3: _t->moveRequested(); break;
        case 4: _t->mkdirRequested(); break;
        case 5: _t->deleteRequested(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (FunctionKeyBar::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FunctionKeyBar::viewRequested)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (FunctionKeyBar::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FunctionKeyBar::editRequested)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (FunctionKeyBar::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FunctionKeyBar::copyRequested)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (FunctionKeyBar::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FunctionKeyBar::moveRequested)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (FunctionKeyBar::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FunctionKeyBar::mkdirRequested)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (FunctionKeyBar::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FunctionKeyBar::deleteRequested)) {
                *result = 5;
                return;
            }
        }
    }
    (void)_a;
}

QT_INIT_METAOBJECT const QMetaObject FunctionKeyBar::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_FunctionKeyBar.data,
    qt_meta_data_FunctionKeyBar,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *FunctionKeyBar::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *FunctionKeyBar::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_FunctionKeyBar.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int FunctionKeyBar::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void FunctionKeyBar::viewRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void FunctionKeyBar::editRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void FunctionKeyBar::copyRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void FunctionKeyBar::moveRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void FunctionKeyBar::mkdirRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void FunctionKeyBar::deleteRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
