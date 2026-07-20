/****************************************************************************
** Meta object code from reading C++ file 'FileOperations.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../operations/FileOperations.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'FileOperations.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_FileOperations_t {
    QByteArrayData data[10];
    char stringdata0[101];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_FileOperations_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_FileOperations_t qt_meta_stringdata_FileOperations = {
    {
QT_MOC_LITERAL(0, 0, 14), // "FileOperations"
QT_MOC_LITERAL(1, 15, 8), // "progress"
QT_MOC_LITERAL(2, 24, 0), // ""
QT_MOC_LITERAL(3, 25, 9), // "doneItems"
QT_MOC_LITERAL(4, 35, 10), // "totalItems"
QT_MOC_LITERAL(5, 46, 9), // "doneBytes"
QT_MOC_LITERAL(6, 56, 10), // "totalBytes"
QT_MOC_LITERAL(7, 67, 11), // "currentFile"
QT_MOC_LITERAL(8, 79, 13), // "errorOccurred"
QT_MOC_LITERAL(9, 93, 7) // "message"

    },
    "FileOperations\0progress\0\0doneItems\0"
    "totalItems\0doneBytes\0totalBytes\0"
    "currentFile\0errorOccurred\0message"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_FileOperations[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       2,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       2,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    5,   24,    2, 0x06 /* Public */,
       8,    1,   35,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::LongLong, QMetaType::LongLong, QMetaType::LongLong, QMetaType::LongLong, QMetaType::QString,    3,    4,    5,    6,    7,
    QMetaType::Void, QMetaType::QString,    9,

       0        // eod
};

void FileOperations::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<FileOperations *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->progress((*reinterpret_cast< qint64(*)>(_a[1])),(*reinterpret_cast< qint64(*)>(_a[2])),(*reinterpret_cast< qint64(*)>(_a[3])),(*reinterpret_cast< qint64(*)>(_a[4])),(*reinterpret_cast< const QString(*)>(_a[5]))); break;
        case 1: _t->errorOccurred((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (FileOperations::*)(qint64 , qint64 , qint64 , qint64 , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FileOperations::progress)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (FileOperations::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FileOperations::errorOccurred)) {
                *result = 1;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject FileOperations::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_FileOperations.data,
    qt_meta_data_FileOperations,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *FileOperations::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *FileOperations::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_FileOperations.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int FileOperations::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 2)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 2)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 2;
    }
    return _id;
}

// SIGNAL 0
void FileOperations::progress(qint64 _t1, qint64 _t2, qint64 _t3, qint64 _t4, const QString & _t5)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void FileOperations::errorOccurred(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
