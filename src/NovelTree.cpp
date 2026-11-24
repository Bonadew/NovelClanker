#include "NovelTree.h"

#include <QDropEvent>

NovelTree::NovelTree(QWidget *parent) : QTreeWidget(parent)
{
    setHeaderHidden(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(true);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
}

int NovelTree::level(const QTreeWidgetItem *item)
{
    int depth = 0;
    for (const QTreeWidgetItem *p = item->parent(); p; p = p->parent()) {
        ++depth;
    }
    return depth;
}

bool NovelTree::dropTarget(const QDropEvent *event, Target &target) const
{
    const QTreeWidgetItem *dragged = currentItem();
    if (!dragged || event->source() != this) {
        return false;
    }
    const int draggedLevel = level(dragged);
    QTreeWidgetItem *over = itemAt(event->position().toPoint());
    const auto indexIn = [this](QTreeWidgetItem *parent, QTreeWidgetItem *child) {
        return parent ? parent->indexOfChild(child) : indexOfTopLevelItem(child);
    };

    if (!over) {
        // Empty space below the last item: only acts can go there (appended).
        if (draggedLevel != 0) {
            return false;
        }
        target = {nullptr, topLevelItemCount()};
        return true;
    }
    if (over == dragged) {
        return false;
    }
    for (const QTreeWidgetItem *p = over->parent(); p; p = p->parent()) {
        if (p == dragged) {
            return false; // can't drop into its own subtree
        }
    }

    const int overLevel = level(over);
    switch (dropIndicatorPosition()) {
    case AboveItem:
    case BelowItem: {
        const int offset = dropIndicatorPosition() == BelowItem ? 1 : 0;
        if (overLevel == draggedLevel) { // between siblings
            target = {over->parent(), indexIn(over->parent(), over) + offset};
            return true;
        }
        if (overLevel == draggedLevel - 1 && offset == 1) { // just below a parent heading: first child
            target = {over, 0};
            return true;
        }
        return false;
    }
    case OnItem:
        if (overLevel == draggedLevel - 1) { // onto a parent: append
            target = {over, over->childCount()};
            return true;
        }
        if (overLevel == draggedLevel) { // onto a sibling: place after it
            target = {over->parent(), indexIn(over->parent(), over) + 1};
            return true;
        }
        return false;
    case OnViewport:
        if (draggedLevel != 0) {
            return false;
        }
        target = {nullptr, topLevelItemCount()};
        return true;
    }
    return false;
}

void NovelTree::dragMoveEvent(QDragMoveEvent *event)
{
    QTreeWidget::dragMoveEvent(event); // updates the drop indicator position
    Target target;
    if (!dropTarget(event, target)) {
        event->ignore();
    }
}

void NovelTree::dropEvent(QDropEvent *event)
{
    Target target;
    QTreeWidgetItem *dragged = currentItem();
    if (!dropTarget(event, target)) {
        event->ignore();
        return;
    }
    // Don't let QTreeWidget move anything; the owner applies the move and rebuilds the tree.
    event->setDropAction(Qt::IgnoreAction);
    event->accept();

    int position = target.index;
    const QTreeWidgetItem *oldParent = dragged->parent();
    const int oldIndex = oldParent ? oldParent->indexOfChild(dragged) : indexOfTopLevelItem(dragged);
    if (oldParent == target.parent && oldIndex < position) {
        --position; // the dragged item is removed first, shifting later siblings up
    }
    if (oldParent == target.parent && oldIndex == position) {
        return; // no change
    }
    emit moveRequested(dragged, target.parent, position);
}
