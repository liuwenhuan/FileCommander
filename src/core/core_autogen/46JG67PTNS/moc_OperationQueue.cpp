/****************************************************************************
** Meta object code from reading C++ file 'OperationQueue.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../operations/OperationQueue.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'OperationQueue.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_OperationQueue_t {
    QByteArrayData data[16];
    char stringdata0[159];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_OperationQueue_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_OperationQueue_t qt_meta_stringdata_OperationQueue = {
    {
QT_MOC_LITERAL(0, 0, 14), // "OperationQueue"
QT_MOC_LITERAL(1, 15, 7), // "started"
QT_MOC_LITERAL(2, 23, 0), // ""
QT_MOC_LITERAL(3, 24, 11), // "description"
QT_MOC_LITERAL(4, 36, 12), // "queueChanged"
QT_MOC_LITERAL(5, 49, 12), // "pendingCount"
QT_MOC_LITERAL(6, 62, 8), // "progress"
QT_MOC_LITERAL(7, 71, 9), // "doneItems"
QT_MOC_LITERAL(8, 81, 10), // "totalItems"
QT_MOC_LITERAL(9, 92, 9), // "doneBytes"
QT_MOC_LITERAL(10, 102, 10), // "totalBytes"
QT_MOC_LITERAL(11, 113, 11), // "currentFile"
QT_MOC_LITERAL(12, 125, 13), // "errorOccurred"
QT_MOC_LITERAL(13, 139, 7), // "message"
QT_MOC_LITERAL(14, 147, 8), // "finished"
QT_MOC_LITERAL(15, 156, 2) // "ok"

    },
    "OperationQueue\0started\0\0description\0"
    "queueChanged\0pendingCount\0progress\0"
    "doneItems\0totalItems\0doneBytes\0"
    "totalBytes\0currentFile\0errorOccurred\0"
    "message\0finished\0ok"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_OperationQueue[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       5,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   39,    2, 0x06 /* Public */,
       4,    1,   42,    2, 0x06 /* Public */,
       6,    5,   45,    2, 0x06 /* Public */,
      12,    1,   56,    2, 0x06 /* Public */,
      14,    1,   59,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::Int,    5,
    QMetaType::Void, QMetaType::LongLong, QMetaType::LongLong, QMetaType::LongLong, QMetaType::LongLong, QMetaType::QString,    7,    8,    9,   10,   11,
    QMetaType::Void, QMetaType::QString,   13,
    QMetaType::Void, QMetaType::Bool,   15,

       0        // eod
};

void OperationQueue::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<OperationQueue *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->started((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->queueChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 2: _t->progress((*reinterpret_cast< qint64(*)>(_a[1])),(*reinterpret_cast< qint64(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3])),(*reinterpret_cast< qint64(*)>(_a[4])),(*reinterpret_cast< const QString(*)>(_a[5]))); break;
        case 3: _t->errorOccurred((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 4: _t->finished((*reinterpret_cast< bool(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (OperationQueue::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OperationQueue::started)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (OperationQueue::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OperationQueue::queueChanged)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (OperationQueue::*)(qint64 , qint64 , qint64 , qint64 , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OperationQueue::progress)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (OperationQueue::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OperationQueue::errorOccurred)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (OperationQueue::*)(bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&OperationQueue::finished)) {
                *result = 4;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject OperationQueue::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_OperationQueue.data,
    qt_meta_data_OperationQueue,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *OperationQueue::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *OperationQueue::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_OperationQueue.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int OperationQueue::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void OperationQueue::started(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void OperationQueue::queueChanged(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void OperationQueue::progress(qint64 _t1, qint64 _t2, qint64 _t3, qint64 _t4, const QString & _t5)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void OperationQueue::errorOccurred(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void OperationQueue::finished(bool _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
