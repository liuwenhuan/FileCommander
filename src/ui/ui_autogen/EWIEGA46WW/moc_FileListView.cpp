/****************************************************************************
** Meta object code from reading C++ file 'FileListView.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../FileListView.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'FileListView.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_FileListView_t {
    QByteArrayData data[11];
    char stringdata0[111];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_FileListView_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_FileListView_t qt_meta_stringdata_FileListView = {
    {
QT_MOC_LITERAL(0, 0, 12), // "FileListView"
QT_MOC_LITERAL(1, 13, 12), // "filesDropped"
QT_MOC_LITERAL(2, 26, 0), // ""
QT_MOC_LITERAL(3, 27, 11), // "sourcePaths"
QT_MOC_LITERAL(4, 39, 7), // "destDir"
QT_MOC_LITERAL(5, 47, 28), // "FileListView::DropActionKind"
QT_MOC_LITERAL(6, 76, 4), // "kind"
QT_MOC_LITERAL(7, 81, 14), // "DropActionKind"
QT_MOC_LITERAL(8, 96, 4), // "Copy"
QT_MOC_LITERAL(9, 101, 4), // "Move"
QT_MOC_LITERAL(10, 106, 4) // "Link"

    },
    "FileListView\0filesDropped\0\0sourcePaths\0"
    "destDir\0FileListView::DropActionKind\0"
    "kind\0DropActionKind\0Copy\0Move\0Link"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_FileListView[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       1,   14, // methods
       0,    0, // properties
       1,   26, // enums/sets
       0,    0, // constructors
       0,       // flags
       1,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    3,   19,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QStringList, QMetaType::QString, 0x80000000 | 5,    3,    4,    6,

 // enums: name, alias, flags, count, data
       7,    7, 0x2,    3,   31,

 // enum data: key, value
       8, uint(FileListView::DropActionKind::Copy),
       9, uint(FileListView::DropActionKind::Move),
      10, uint(FileListView::DropActionKind::Link),

       0        // eod
};

void FileListView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<FileListView *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->filesDropped((*reinterpret_cast< const QStringList(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< FileListView::DropActionKind(*)>(_a[3]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (FileListView::*)(const QStringList & , const QString & , FileListView::DropActionKind );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&FileListView::filesDropped)) {
                *result = 0;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject FileListView::staticMetaObject = { {
    QMetaObject::SuperData::link<QTableView::staticMetaObject>(),
    qt_meta_stringdata_FileListView.data,
    qt_meta_data_FileListView,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *FileListView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *FileListView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_FileListView.stringdata0))
        return static_cast<void*>(this);
    return QTableView::qt_metacast(_clname);
}

int FileListView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QTableView::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 1)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 1;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 1)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 1;
    }
    return _id;
}

// SIGNAL 0
void FileListView::filesDropped(const QStringList & _t1, const QString & _t2, FileListView::DropActionKind _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
