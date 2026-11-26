#pragma once

#include <QTreeWidget>

// Tree of acts (top level) -> chapters -> scenes.
//
// Drag and drop only *requests* moves via moveRequested(); the tree itself is never rearranged by a drop.
// The owner applies the move to the novel and rebuilds the tree, so storage stays the single source of
// truth. Drops that would break the hierarchy (e.g. a scene directly under an act) are refused.
class NovelTree : public QTreeWidget {
  Q_OBJECT

public:
  explicit NovelTree(QWidget *parent = nullptr);

  // Depth of an item: 0 = act, 1 = chapter, 2 = scene.
  static int level(const QTreeWidgetItem *item);

signals:
  // Move `item` under `newParent` (nullptr for acts) so it ends up at `position` among its new
  // siblings, counted after the item has been taken out of its current place.
  void moveRequested(QTreeWidgetItem *item, QTreeWidgetItem *newParent, int position);

protected:
  void dragMoveEvent(QDragMoveEvent *event) override;
  void dropEvent(QDropEvent *event) override;

private:
  struct Target {
    QTreeWidgetItem *parent = nullptr;
    int index = -1; // insertion index among parent's children, before removing the dragged item
  };

  bool dropTarget(const QDropEvent *event, Target &target) const;
};
