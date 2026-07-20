/****************************************************************************
** Meta object code from reading C++ file 'MainWindow.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.8)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../MainWindow.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'MainWindow.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.8. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_MainWindow_t {
    QByteArrayData data[54];
    char stringdata0[757];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_MainWindow_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_MainWindow_t qt_meta_stringdata_MainWindow = {
    {
QT_MOC_LITERAL(0, 0, 10), // "MainWindow"
QT_MOC_LITERAL(1, 11, 14), // "setActivePanel"
QT_MOC_LITERAL(2, 26, 0), // ""
QT_MOC_LITERAL(3, 27, 10), // "FilePanel*"
QT_MOC_LITERAL(4, 38, 5), // "panel"
QT_MOC_LITERAL(5, 44, 11), // "viewCurrent"
QT_MOC_LITERAL(6, 56, 11), // "editCurrent"
QT_MOC_LITERAL(7, 68, 12), // "copySelected"
QT_MOC_LITERAL(8, 81, 12), // "moveSelected"
QT_MOC_LITERAL(9, 94, 13), // "makeDirectory"
QT_MOC_LITERAL(10, 108, 14), // "deleteSelected"
QT_MOC_LITERAL(11, 123, 9), // "permanent"
QT_MOC_LITERAL(12, 133, 13), // "renameCurrent"
QT_MOC_LITERAL(13, 147, 16), // "compressSelected"
QT_MOC_LITERAL(14, 164, 10), // "openSearch"
QT_MOC_LITERAL(15, 175, 19), // "openShortcutsDialog"
QT_MOC_LITERAL(16, 195, 8), // "setTheme"
QT_MOC_LITERAL(17, 204, 15), // "Settings::Theme"
QT_MOC_LITERAL(18, 220, 5), // "theme"
QT_MOC_LITERAL(19, 226, 11), // "setLanguage"
QT_MOC_LITERAL(20, 238, 8), // "language"
QT_MOC_LITERAL(21, 247, 21), // "openMultiRenameDialog"
QT_MOC_LITERAL(22, 269, 14), // "openSyncDialog"
QT_MOC_LITERAL(23, 284, 20), // "compareSelectedFiles"
QT_MOC_LITERAL(24, 305, 18), // "compareDirectories"
QT_MOC_LITERAL(25, 324, 20), // "openDirectoryHotlist"
QT_MOC_LITERAL(26, 345, 14), // "showProperties"
QT_MOC_LITERAL(27, 360, 14), // "calculateSizes"
QT_MOC_LITERAL(28, 375, 22), // "syncOtherPanelToActive"
QT_MOC_LITERAL(29, 398, 10), // "swapPanels"
QT_MOC_LITERAL(30, 409, 9), // "splitFile"
QT_MOC_LITERAL(31, 419, 12), // "combineFiles"
QT_MOC_LITERAL(32, 432, 16), // "openTerminalHere"
QT_MOC_LITERAL(33, 449, 15), // "openWithDefault"
QT_MOC_LITERAL(34, 465, 8), // "openWith"
QT_MOC_LITERAL(35, 474, 15), // "toggleQuickView"
QT_MOC_LITERAL(36, 490, 15), // "updateQuickView"
QT_MOC_LITERAL(37, 506, 8), // "undoLast"
QT_MOC_LITERAL(38, 515, 10), // "runCommand"
QT_MOC_LITERAL(39, 526, 7), // "command"
QT_MOC_LITERAL(40, 534, 9), // "directory"
QT_MOC_LITERAL(41, 544, 16), // "toggleFolderTree"
QT_MOC_LITERAL(42, 561, 12), // "navigateBack"
QT_MOC_LITERAL(43, 574, 15), // "navigateForward"
QT_MOC_LITERAL(44, 590, 10), // "navigateUp"
QT_MOC_LITERAL(45, 601, 18), // "refreshActivePanel"
QT_MOC_LITERAL(46, 620, 18), // "handleFilesDropped"
QT_MOC_LITERAL(47, 639, 7), // "sources"
QT_MOC_LITERAL(48, 647, 7), // "destDir"
QT_MOC_LITERAL(49, 655, 28), // "FileListView::DropActionKind"
QT_MOC_LITERAL(50, 684, 4), // "kind"
QT_MOC_LITERAL(51, 689, 24), // "copySelectionToClipboard"
QT_MOC_LITERAL(52, 714, 23), // "cutSelectionToClipboard"
QT_MOC_LITERAL(53, 738, 18) // "pasteFromClipboard"

    },
    "MainWindow\0setActivePanel\0\0FilePanel*\0"
    "panel\0viewCurrent\0editCurrent\0"
    "copySelected\0moveSelected\0makeDirectory\0"
    "deleteSelected\0permanent\0renameCurrent\0"
    "compressSelected\0openSearch\0"
    "openShortcutsDialog\0setTheme\0"
    "Settings::Theme\0theme\0setLanguage\0"
    "language\0openMultiRenameDialog\0"
    "openSyncDialog\0compareSelectedFiles\0"
    "compareDirectories\0openDirectoryHotlist\0"
    "showProperties\0calculateSizes\0"
    "syncOtherPanelToActive\0swapPanels\0"
    "splitFile\0combineFiles\0openTerminalHere\0"
    "openWithDefault\0openWith\0toggleQuickView\0"
    "updateQuickView\0undoLast\0runCommand\0"
    "command\0directory\0toggleFolderTree\0"
    "navigateBack\0navigateForward\0navigateUp\0"
    "refreshActivePanel\0handleFilesDropped\0"
    "sources\0destDir\0FileListView::DropActionKind\0"
    "kind\0copySelectionToClipboard\0"
    "cutSelectionToClipboard\0pasteFromClipboard"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_MainWindow[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      41,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       0,       // signalCount

 // slots: name, argc, parameters, tag, flags
       1,    1,  219,    2, 0x08 /* Private */,
       5,    0,  222,    2, 0x08 /* Private */,
       6,    0,  223,    2, 0x08 /* Private */,
       7,    0,  224,    2, 0x08 /* Private */,
       8,    0,  225,    2, 0x08 /* Private */,
       9,    0,  226,    2, 0x08 /* Private */,
      10,    1,  227,    2, 0x08 /* Private */,
      10,    0,  230,    2, 0x28 /* Private | MethodCloned */,
      12,    0,  231,    2, 0x08 /* Private */,
      13,    0,  232,    2, 0x08 /* Private */,
      14,    0,  233,    2, 0x08 /* Private */,
      15,    0,  234,    2, 0x08 /* Private */,
      16,    1,  235,    2, 0x08 /* Private */,
      19,    1,  238,    2, 0x08 /* Private */,
      21,    0,  241,    2, 0x08 /* Private */,
      22,    0,  242,    2, 0x08 /* Private */,
      23,    0,  243,    2, 0x08 /* Private */,
      24,    0,  244,    2, 0x08 /* Private */,
      25,    0,  245,    2, 0x08 /* Private */,
      26,    0,  246,    2, 0x08 /* Private */,
      27,    0,  247,    2, 0x08 /* Private */,
      28,    0,  248,    2, 0x08 /* Private */,
      29,    0,  249,    2, 0x08 /* Private */,
      30,    0,  250,    2, 0x08 /* Private */,
      31,    0,  251,    2, 0x08 /* Private */,
      32,    0,  252,    2, 0x08 /* Private */,
      33,    0,  253,    2, 0x08 /* Private */,
      34,    0,  254,    2, 0x08 /* Private */,
      35,    0,  255,    2, 0x08 /* Private */,
      36,    0,  256,    2, 0x08 /* Private */,
      37,    0,  257,    2, 0x08 /* Private */,
      38,    2,  258,    2, 0x08 /* Private */,
      41,    0,  263,    2, 0x08 /* Private */,
      42,    0,  264,    2, 0x08 /* Private */,
      43,    0,  265,    2, 0x08 /* Private */,
      44,    0,  266,    2, 0x08 /* Private */,
      45,    0,  267,    2, 0x08 /* Private */,
      46,    3,  268,    2, 0x08 /* Private */,
      51,    0,  275,    2, 0x08 /* Private */,
      52,    0,  276,    2, 0x08 /* Private */,
      53,    0,  277,    2, 0x08 /* Private */,

 // slots: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool,   11,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 17,   18,
    QMetaType::Void, QMetaType::QString,   20,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString, QMetaType::QString,   39,   40,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QStringList, QMetaType::QString, 0x80000000 | 49,   47,   48,   50,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void MainWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<MainWindow *>(_o);
        (void)_t;
        switch (_id) {
        case 0: _t->setActivePanel((*reinterpret_cast< FilePanel*(*)>(_a[1]))); break;
        case 1: _t->viewCurrent(); break;
        case 2: _t->editCurrent(); break;
        case 3: _t->copySelected(); break;
        case 4: _t->moveSelected(); break;
        case 5: _t->makeDirectory(); break;
        case 6: _t->deleteSelected((*reinterpret_cast< bool(*)>(_a[1]))); break;
        case 7: _t->deleteSelected(); break;
        case 8: _t->renameCurrent(); break;
        case 9: _t->compressSelected(); break;
        case 10: _t->openSearch(); break;
        case 11: _t->openShortcutsDialog(); break;
        case 12: _t->setTheme((*reinterpret_cast< Settings::Theme(*)>(_a[1]))); break;
        case 13: _t->setLanguage((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 14: _t->openMultiRenameDialog(); break;
        case 15: _t->openSyncDialog(); break;
        case 16: _t->compareSelectedFiles(); break;
        case 17: _t->compareDirectories(); break;
        case 18: _t->openDirectoryHotlist(); break;
        case 19: _t->showProperties(); break;
        case 20: _t->calculateSizes(); break;
        case 21: _t->syncOtherPanelToActive(); break;
        case 22: _t->swapPanels(); break;
        case 23: _t->splitFile(); break;
        case 24: _t->combineFiles(); break;
        case 25: _t->openTerminalHere(); break;
        case 26: _t->openWithDefault(); break;
        case 27: _t->openWith(); break;
        case 28: _t->toggleQuickView(); break;
        case 29: _t->updateQuickView(); break;
        case 30: _t->undoLast(); break;
        case 31: _t->runCommand((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 32: _t->toggleFolderTree(); break;
        case 33: _t->navigateBack(); break;
        case 34: _t->navigateForward(); break;
        case 35: _t->navigateUp(); break;
        case 36: _t->refreshActivePanel(); break;
        case 37: _t->handleFilesDropped((*reinterpret_cast< const QStringList(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< FileListView::DropActionKind(*)>(_a[3]))); break;
        case 38: _t->copySelectionToClipboard(); break;
        case 39: _t->cutSelectionToClipboard(); break;
        case 40: _t->pasteFromClipboard(); break;
        default: ;
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject MainWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_MainWindow.data,
    qt_meta_data_MainWindow,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *MainWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MainWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_MainWindow.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int MainWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 41)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 41;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 41)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 41;
    }
    return _id;
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
