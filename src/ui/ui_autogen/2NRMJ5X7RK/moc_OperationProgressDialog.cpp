/****************************************************************************
** Meta object code from reading C++ file 'OperationProgressDialog.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../dialogs/OperationProgressDialog.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'OperationProgressDialog.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_OperationProgressDialog_t {
    QByteArrayData data[15];
    char stringdata0[188];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_OperationProgressDialog_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_OperationProgressDialog_t qt_meta_stringdata_OperationProgressDialog = {
    {
QT_MOC_LITERAL(0, 0, 23), // "OperationProgressDialog"
QT_MOC_LITERAL(1, 24, 15), // "cancelRequested"
QT_MOC_LITERAL(2, 40, 0), // ""
QT_MOC_LITERAL(3, 41, 14), // "pauseRequested"
QT_MOC_LITERAL(4, 56, 15), // "resumeRequested"
QT_MOC_LITERAL(5, 72, 14), // "setDescription"
QT_MOC_LITERAL(6, 87, 11), // "description"
QT_MOC_LITERAL(7, 99, 11), // "setProgress"
QT_MOC_LITERAL(8, 111, 9), // "doneItems"
QT_MOC_LITERAL(9, 121, 10), // "totalItems"
QT_MOC_LITERAL(10, 132, 9), // "doneBytes"
QT_MOC_LITERAL(11, 142, 10), // "totalBytes"
QT_MOC_LITERAL(12, 153, 11), // "currentFile"
QT_MOC_LITERAL(13, 165, 14), // "setQueuedCount"
QT_MOC_LITERAL(14, 180, 7) // "pending"

    },
    "OperationProgressDialog\0cancelRequested\0"
    "\0pauseRequested\0resumeRequested\0"
    "setDescription\0description\0setProgress\0"
    "doneItems\0totalItems\0doneBytes\0"
    "totalBytes\0currentFile\0setQueuedCount\0"
    "pending"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_OperationProgressDialog[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       6,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       3,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    0,   44,    2, 0x06 /* Public */,
       3,    0,   45,    2, 0x06 /* Public */,
       4,    0,   46,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       5,    1,   47,    2, 0x0a /* Public */,
       7,    5,   50,    2, 0x0a /* Public */,
      13,    1,   61,    2, 0x0a /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

 // slots: parameters
    QMetaType::Void, QMetaType::QString,    6,
    QMetaType::Void, QMetaType::LongLong, QMetaType::LongLong, QMetaType::LongLong, QMetaType::LongLong, QMetaType::QString,    8,    9,   10,   11,   12,
    QMetaType::Void, QMetaType::Int,   14,

       0        // eod
};

void OperationProgressDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<OperationProgressDialog *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->cancelRequested(); break;
        case 1: _t->pauseRequested(); break;
        case 2: _t->resumeRequested(); break;
        case 3: _t->setDescription((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: _t->setProgress((*reinterpret_cast< qint64(*)>(_a[1])),(*reinterpret_cast< qint64(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3])),(*reinterpret_cast< qint64(*)>(_a[4])),(*reinterpret_cast< const QString(*)>(_a[5]))); break;
        case 5: _t->setQueuedCount((*reinterpret_cast< int(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (OperationProgressDialog::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OperationProgressDialog::cancelRequested)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (OperationProgressDialog::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OperationProgressDialog::pauseRequested)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (OperationProgressDialog::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OperationProgressDialog::resumeRequested)) {
                *result = 2;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject OperationProgressDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_meta_stringdata_OperationProgressDialog.data,
    qt_meta_data_OperationProgressDialog,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *OperationProgressDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *OperationProgressDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_OperationProgressDialog.stringdata0))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int OperationProgressDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDialog::qt_metacall(_c, _id, _a);
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
void OperationProgressDialog::cancelRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void OperationProgressDialog::pauseRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void OperationProgressDialog::resumeRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
