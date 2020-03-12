#include <QPen>
#include <QBrush>
#include <QPainter>
#include <QMap>
#include <QGraphicsSceneHoverEvent>
#include <QApplication>
#include <QMetaEnum>
#include <QVector2D>
#include <QtMath>
#include <QMenu>
#include "wire.h"
#include "connector.h"
#include "scene.h"
#include "label.h"
#include "node.h"
#include "../utils.h"
#include "../commands/commandwirepointmove.h"

const qreal BOUNDING_RECT_PADDING = 6.0;
const qreal HANDLE_SIZE = 3.0;
const qreal WIRE_SHAPE_PADDING = 10;
const QColor COLOR                     = QColor("#000000");
const QColor COLOR_HIGHLIGHTED         = QColor("#dc2479");
const QColor COLOR_SELECTED            = QColor("#0f16af");

using namespace QSchematic;

class PointWithIndex {
public:
    PointWithIndex(int index, const QPoint& point) : index(index), point(point) {}
    int index;
    QPoint point;

    bool operator<(const PointWithIndex& other) const {
        return index < other.index;
    }
};

Wire::Wire(int type, QGraphicsItem* parent) :
    Item(type, parent), _renameAction(nullptr), _internalMove(false)
{
    _pointToMoveIndex = -1;
    _lineSegmentToMoveIndex = -1;

    // Lines should always be the lowest item in Z-Order
    setZValue(-10);

    // ALWAYS snap to grid
    setSnapToGrid(true);
    setMovable(true);
}

Wire::~Wire()
{
    // Make sure that we don't delete the net's label
    if (childItems().contains(net()->label().get())) {
        net()->label()->setParentItem(nullptr);
    }
}

gpds::container Wire::to_container() const
{
    // Points
    gpds::container pointsContainer;
    for (int i = 0; i < points_count(); i++) {
        gpds::container pointContainer;
        pointContainer.add_attribute("index", i);
        pointContainer.add_value("x", _points.at(i).x());
        pointContainer.add_value("y", _points.at(i).y());
        pointsContainer.add_value("point", pointContainer);
    }

    // Root
    gpds::container rootContainer;
    addItemTypeIdToContainer(rootContainer);
    rootContainer.add_value("item", Item::to_container());
    rootContainer.add_value("points", pointsContainer);

    return rootContainer;
}

void Wire::from_container(const gpds::container& container)
{
    // Root
    Item::from_container(*container.get_value<gpds::container*>("item").value());

    // Points
    const gpds::container* pointsContainer = container.get_value<gpds::container*>("points").value_or(nullptr);
    if (pointsContainer) {
        auto points = pointsContainer->get_values<gpds::container*>("point");
        // Sort points by index
        std::sort(points.begin(), points.end(), [](gpds::container* a, gpds::container* b) {
            std::optional<int> index1 = a->get_attribute<int>("index");
            std::optional<int> index2 = b->get_attribute<int>("index");
            if (!index1.has_value() || !index2.has_value()) {
                qCritical("Wire::from_container(): Point has no index.");
                return false;
            }
            return index1.value() < index2.value();
        });
        for (const gpds::container* pointContainer : points ) {
            _points.append(WirePoint(pointContainer->get_value<double>("x").value_or(0),
                                     pointContainer->get_value<double>("y").value_or(0)));
        }
    }

    update();
}

std::shared_ptr<Item> Wire::deepCopy() const
{
    auto clone = std::make_shared<Wire>(type(), parentItem());
    copyAttributes(*(clone.get()));

    return clone;
}

void Wire::copyAttributes(Wire& dest) const
{
    Item::copyAttributes(dest);

    dest._points = _points;
    dest._rect = _rect;
    dest._pointToMoveIndex = _pointToMoveIndex;
    dest._lineSegmentToMoveIndex = _lineSegmentToMoveIndex;
    dest._prevMousePos = _prevMousePos;
}

void Wire::update()
{
    calculateBoundingRect();

    Item::update();
}

QRectF Wire::boundingRect() const
{
    return _rect.adjusted(-BOUNDING_RECT_PADDING, -BOUNDING_RECT_PADDING, BOUNDING_RECT_PADDING, BOUNDING_RECT_PADDING);
}

QPainterPath Wire::shape() const
{
    QPainterPath basePath;
    basePath.addPolygon(QPolygonF(pointsRelative()));

    QPainterPathStroker str;
    str.setCapStyle(Qt::FlatCap);
    str.setJoinStyle(Qt::MiterJoin);
    str.setWidth(WIRE_SHAPE_PADDING);

    QPainterPath resultPath = str.createStroke(basePath).simplified();

    return resultPath;
}

QVector<WirePoint> Wire::wirePointsRelative() const
{
    QVector<WirePoint> relativePoints(_points);

    for (WirePoint& point : relativePoints) {
        bool isJunction = point.isJunction();
        point = point.toPointF() - pos();
        point.setIsJunction(isJunction);
    }

    return relativePoints;
}

QVector<QPointF> Wire::pointsRelative() const
{
    QVector<QPointF> points;

    for (const WirePoint& point : _points) {
        points << point.toPointF() - pos();
    }

    return points;
}

QVector<QPointF> Wire::pointsAbsolute() const
{
    QVector<QPointF> points;

    for (const WirePoint& point : _points) {
        points << point.toPointF();
    }

    return points;
}

void Wire::calculateBoundingRect()
{
    // Find top-left most point
    const int& intMaxValue = std::numeric_limits<int>::max();
    QPointF topLeft(intMaxValue, intMaxValue);
    for (auto& point : wirePointsRelative()) {
        if (point.x() < topLeft.x())
            topLeft.setX(point.x());
        if (point.y() < topLeft.y())
            topLeft.setY(point.y());
    }

    // Find bottom-right most point
    const int& intMinValue = std::numeric_limits<int>::min();
    QPointF bottomRight(intMinValue, intMinValue);
    for (auto& point : wirePointsRelative()) {
        if (point.x() > bottomRight.x())
            bottomRight.setX(point.x());
        if (point.y() > bottomRight.y())
            bottomRight.setY(point.y());
    }

    // Create the rectangle
    _rect = QRectF(topLeft, bottomRight);
    if (movingWirePoint() and not topLeft.isNull()) {
        updatePosition();
    }
}

void Wire::updatePosition()
{
// TODO: Not sure what has to be done here
//    QPointF topLeft = _rect.topLeft();
//    for (int i = 0; i < points_count(); i++) {
//        _points[i].setX(_points[i].x() - topLeft.x());
//        _points[i].setY(_points[i].y() - topLeft.y());
//    }
//    // Move all the child items
//    for (auto& item : childItems()) {
//        item->setPos(item->pos() - topLeft);
//    }
//    QPointF newPos = pos() + topLeft;
//    QPointF snappedPos = _settings.snapToGrid(newPos);
//    _offset = newPos - snappedPos;
//    _internalMove = true;
//    setPos(newPos);
//    _internalMove = false;
//    calculateBoundingRect();
}

void Wire::setRenameAction(QAction* action)
{
    _renameAction = action;
}

void Wire::prepend_point(const QPointF& point)
{
    wire::prepend_point(point);
    emit pointMoved(*this, wirePointsRelative().first());
}

void Wire::append_point(const QPointF& point)
{
    wire::append_point(point);
    emit pointMoved(*this, wirePointsRelative().last());
}

void Wire::insert_point(int index, const QPointF& point)
{
    wire::insert_point(index, point);
    emit pointMoved(*this, wirePointsRelative()[index]);
}

void Wire::removeFirstPoint()
{
    if (points_count() <= 0) {
        return;
    }
    prepareGeometryChange();
    _points.removeFirst();
    calculateBoundingRect();
}

void Wire::removeLastPoint()
{
    if (points_count() <= 0) {
        return;
    }

    prepareGeometryChange();
    _points.removeLast();
    calculateBoundingRect();
}

void Wire::removePoint(int index)
{
    prepareGeometryChange();
    // Move the junction on the previous and next segments
    if (index > 0 and index < points_count() - 1) {
        Line newSegment(pointsAbsolute().at(index - 1), pointsAbsolute().at(index + 1));
        move_junctions_to_new_segment(line_segments().at(index - 1), newSegment);
        move_junctions_to_new_segment(line_segments().at(index), newSegment);
    } else {
        for (const auto& wire: connected_wires()) {
            for (int junctionIndex: wire->junctions()) {
                QPointF point = wire->points().at(junctionIndex).toPointF();
                if (line_segments().first().containsPoint(point)) {
                    wire->move_point_to(junctionIndex, pointsAbsolute().at(1));
                }
                if (line_segments().last().containsPoint(point)) {
                    wire->move_point_to(junctionIndex, pointsAbsolute().at(pointsAbsolute().count() - 2));
                }
            }
        }
    }
    _points.remove(index);
    calculateBoundingRect();
    emit pointRemoved(index);
}

void Wire::simplify()
{
    prepareGeometryChange();
    removeDuplicatePoints();
    removeObsoletePoints();
    calculateBoundingRect();
}

void Wire::removeDuplicatePoints()
{
    int i = 0;
    while (i < points_count() - 1 and points_count() > 2) {
        WirePoint p1 = wirePointsRelative().at(i);
        WirePoint p2 = wirePointsRelative().at(i+1);

        // Check if p2 is the same as p1
        if (p1 == p2) {
            // If p1 is not a junction itself then inherit from p2
            if (!p1.isJunction()) {
                set_point_is_junction(i, p2.isJunction());
            }
            emit pointRemoved(i+1);
            _points.removeAt(i+1);
        } else {
            i++;
        }
    }
}

void Wire::removeObsoletePoints()
{
    // Don't do anything if there are not at least three line segments
    if (points_count() < 3) {
        return;
    }

    // Compile a list of obsolete points
    auto it = _points.begin()+2;
    while (it != _points.end()) {
        QPointF p1 = (*(it - 2)).toPointF();
        QPointF p2 = (*(it - 1)).toPointF();
        QPointF p3 = (*it).toPointF();

        // Check if p2 is on the line created by p1 and p3
        if (Utils::pointIsOnLine(QLineF(p1, p2), p3)) {
            emit pointRemoved(_points.indexOf(*(it-1)));
            it = _points.erase(it-1);
        }
        it++;
    }
}

void Wire::move_point_to(int index, const QPointF& moveTo)
{
    prepareGeometryChange();
    wire_system::wire::move_point_to(index, moveTo);

    emit pointMoved(*this, wirePointsRelative()[index]);
    calculateBoundingRect();
    update();
}

bool Wire::connectWire(Wire* wire)
{
    if (_connectedWires.contains(wire)) {
        return false;
    }
    _connectedWires.append(wire);
    return true;
}

void Wire::disconnectWire(Wire* wire)
{
    _connectedWires.removeAll(wire);
}

void Wire::mousePressEvent(QGraphicsSceneMouseEvent* event)
{
    // Check wheter we clicked on a handle
    if (isSelected()) {
        // Check whether we clicked on a handle
        QVector<QPointF> points(pointsAbsolute());
        _pointToMoveIndex = -1;
        for (int i = 0; i < points.count(); i++) {
            QRectF handleRect(points.at(i).x() - HANDLE_SIZE, points.at(i).y() - HANDLE_SIZE, 2*HANDLE_SIZE, 2*HANDLE_SIZE);

            if (handleRect.contains(event->scenePos())) {
                _pointToMoveIndex = i;
                setMovable(false);
                break;
            }
        }

        // Check whether we clicked on a line segment
        QList<Line> lines = line_segments();
        for (int i = 0; i < lines.count(); i++) {
            const Line& line = lines.at(i);
            if (line.containsPoint(event->scenePos(), 1)) {
                _lineSegmentToMoveIndex = i;
                setMovable(false);
                break;
            }

            _lineSegmentToMoveIndex = -1;
        }

    } else {
        Item::mousePressEvent(event);
    }

    // Store last known mouse pos
    _prevMousePos = event->scenePos();
}

void Wire::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
    Item::mouseReleaseEvent(event);

    _pointToMoveIndex = -1;
    _lineSegmentToMoveIndex = -1;
    setMovable(true);

    // Store last known mouse pos
    _prevMousePos = event->scenePos();
    simplify();
}

void Wire::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
    QPointF curPos = event->scenePos();
    bool ctrlPressed = QApplication::keyboardModifiers() & Qt::ControlModifier;

    // Snap to grid (if supposed to)
    if (snapToGrid()) {
        curPos = _settings.snapToGrid(curPos);
    }

    // Move a point?
    if (_pointToMoveIndex > -1) {
        // Yep, we can do this
        event->accept();

        // Move
        auto wire = this->sharedPtr<Wire>();
        auto command = new CommandWirepointMove(scene(), wire, _pointToMoveIndex, curPos);
        scene()->undoStack()->push(command);
    }

    // Move a line segment?
    else if (_lineSegmentToMoveIndex > -1){
        // Yep, we can do this
        event->accept();

        // Determine movement vector
        const Line line = line_segments().at(_lineSegmentToMoveIndex);
        QVector2D moveLineBy(0, 0);
        if (line.isHorizontal()) {
            moveLineBy = QVector2D(0, static_cast<float>(curPos.y() - _prevMousePos.y()));
        } else if (line.isVertical()) {
            moveLineBy = QVector2D(static_cast<float>(curPos.x() - _prevMousePos.x()), 0);
        } else if (ctrlPressed) {
            moveLineBy = QVector2D(curPos - _prevMousePos);
        }

        // Snap to grid (if supposed to)
        if (snapToGrid()) {
            moveLineBy = _settings.snapToGrid(moveLineBy);
        }

        // Move line segment
        move_line_segment_by(_lineSegmentToMoveIndex, moveLineBy);
    }

    // Nothing interesting for us to do
    else {

        // Hand over to base class
        Item::mouseMoveEvent(event);
    }

    // Store last known mouse pos
    _prevMousePos = curPos;
}

void Wire::hoverEnterEvent(QGraphicsSceneHoverEvent* event)
{
    Item::hoverEnterEvent(event);
}

void Wire::hoverLeaveEvent(QGraphicsSceneHoverEvent* event)
{
    Item::hoverLeaveEvent(event);

    unsetCursor();
}

void Wire::hoverMoveEvent(QGraphicsSceneHoverEvent* event)
{
    Item::hoverMoveEvent(event);

    // Only if wire is selected
    if (!isSelected()) {
        return;
    }

    // Check whether we hover over a point handle
    QVector<QPointF> points(pointsAbsolute());
    for (int i = 0; i < points.count(); i++) {
        QRectF handleRect(points.at(i).x() - HANDLE_SIZE, points.at(i).y() - HANDLE_SIZE, 2*HANDLE_SIZE, 2*HANDLE_SIZE);

        if (handleRect.contains(event->scenePos())) {
            setCursor(Qt::SizeAllCursor);
            return;
        }
    }

    // Check whether we hover over a line segment
    bool ctrlPressed = QApplication::keyboardModifiers() & Qt::ControlModifier;
    QList<Line> lines = line_segments();
    for (int i = 0; i < lines.count(); i++) {
        // Retrieve the line segment
        const Line& line = lines.at(i);

        // Set the appropriate cursor
        if (line.containsPoint(event->scenePos(), 1)) {
            if (line.isHorizontal()) {
                setCursor(Qt::SizeVerCursor);
            } else if (line.isVertical()) {
                setCursor(Qt::SizeHorCursor);
            } else if (ctrlPressed) {
                setCursor(Qt::SizeAllCursor);
            }
            return;
        }
    }

    unsetCursor();
}

void Wire::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(option);
    Q_UNUSED(widget);

    QPen penLine;
    penLine.setStyle(Qt::SolidLine);
    penLine.setCapStyle(Qt::RoundCap);
    QColor penColor;
    if (isSelected()) {
        penColor = COLOR_SELECTED;
    } else if (isHighlighted()) {
        penColor = COLOR_HIGHLIGHTED;
    } else {
        penColor = COLOR;
    }
    penLine.setWidth(1);
    penLine.setColor(penColor);

    QBrush brushLine;
    brushLine.setStyle(Qt::NoBrush);

    QPen penJunction;
    penJunction.setStyle(Qt::NoPen);

    QBrush brushJunction;
    brushJunction.setStyle(Qt::SolidPattern);
    brushJunction.setColor(isHighlighted() ? COLOR_HIGHLIGHTED : COLOR);

    QPen penHandle;
    penHandle.setColor(Qt::black);
    penHandle.setStyle(Qt::SolidLine);

    QBrush brushHandle;
    brushHandle.setColor(Qt::black);
    brushHandle.setStyle(Qt::SolidPattern);

    // Draw the actual line
    painter->setPen(penLine);
    painter->setBrush(brushLine);
    const auto& points = pointsRelative();
    painter->drawPolyline(points.constData(), points.count());

    // Draw the junction poins
    int junctionRadius = 4;
    for (const WirePoint& wirePoint : wirePointsRelative()) {
        if (wirePoint.isJunction()) {
            painter->setPen(penJunction);
            painter->setBrush(brushJunction);
            painter->drawEllipse(wirePoint.toPointF(), junctionRadius, junctionRadius);
        }
    }

    // Draw the handles (if selected)
    if (isSelected()) {
        painter->setOpacity(0.5);
        painter->setPen(penHandle);
        painter->setBrush(brushHandle);
        for (const QPointF& point : points) {
            QRectF handleRect(point.x() - HANDLE_SIZE, point.y() - HANDLE_SIZE, 2*HANDLE_SIZE, 2*HANDLE_SIZE);
            painter->drawRect(handleRect);
        }
    }

    // Draw debugging stuff
    if (_settings.debug) {
        painter->setPen(Qt::red);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(boundingRect());

        painter->setPen(Qt::blue);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(shape());
    }
}

QVariant Wire::itemChange(QGraphicsItem::GraphicsItemChange change, const QVariant& value)
{
    switch (change) {

    case ItemPositionChange: {
        // Move the wire
        QPointF newPos = QPointF(_settings.snapToGrid(value.toPointF())) + _offset;
        QVector2D movedBy = QVector2D(newPos - pos());
        move(movedBy);
        return newPos;
    }
    case ItemPositionHasChanged:
        if (_internalMove) {
            break;
        }
        if (not scene()) {
            break;
        }
        // Move points to their connectors
        for (const auto& conn : scene()->connectors()) {
            bool isSelected = false;
            // Check if the connector's node is selected
            for (const auto& item : scene()->selectedTopLevelItems()) {
                auto node = item->sharedPtr<Node>();
                if (node) {
                    if (node->connectors().contains(conn)) {
                        isSelected = true;
                        break;
                    }
                }
            }
            // Move point onto the connector
            if (not isSelected and scene()->wireSystem()->attachedWire(conn).get() == this) {
                int index = scene()->wireSystem()->attachedWirepoint(conn);
                QVector2D moveBy(conn->scenePos() - pointsAbsolute().at(index));
                move_point_by(index, moveBy);
            }
        }
        break;
    case ItemSelectedHasChanged:
        if (value.toBool()) {
            setZValue(zValue()+1);
        } else {
            setZValue(zValue()-1);
        }
        break;
    default:
        return Item::itemChange(change, value);
    }
    return Item::itemChange(change, value);
}

void Wire::contextMenuEvent(QGraphicsSceneContextMenuEvent* event)
{
    QMenu menu;
    QAction* actionAdd = menu.addAction("Add point");
    // If there is a point nearby
    int pointIndex = -1;
    for (int i = 0; i < points_count(); i++) {
        if (QVector2D(pointsAbsolute().at(i)).distanceToPoint(QVector2D(event->scenePos())) < 5) {
            pointIndex = i;
            break;
        }
    }
    QAction* actionRemove = nullptr;
    if (points_count() > 2 and pointIndex != -1) {
        actionRemove = menu.addAction("Remove point");
    }
    if (_renameAction) {
        menu.addAction(_renameAction);
    }
    if (not net()->label()->text().isEmpty()) {
        QAction* showAction = menu.addAction("Label visible");
        showAction->setCheckable(true);
        showAction->setChecked(net()->label()->isVisible());

        connect(showAction, &QAction::triggered, this, &Wire::toggleLabelRequested);
    }
    bool labelWasVisible = net()->label()->isVisible();
    QAction* command = menu.exec(event->screenPos());

    // Add a point at the cursor
    if (command == actionAdd) {
        for (int i = 0; i < line_segments().count(); i++) {
            if (line_segments().at(i).containsPoint(event->scenePos(), 4)) {
                setSelected(true);
                insert_point(i + 1, _settings.snapToGrid(event->scenePos()));
                break;
            }
        }
    }

    // Remove the point near the cursor
    if (actionRemove and command == actionRemove) {
        removePoint(pointIndex);
    }

    // Move the label to the cursor if it was just made visible
    if (not labelWasVisible and net()->label()->isVisible()) {
        // Find line segment
        Line seg;
        QList<Line> lines = line_segments();
        for (const auto& line : lines) {
            if (line.containsPoint(event->scenePos(), WIRE_SHAPE_PADDING/2)) {
                seg = line;
                break;
            }

        }
        // This should never happen
        if (seg.isNull()) {
            qCritical("Wire::contextMenuEvent(): Couldn't identify the segment the user clicked on.");
            return;
        }
        // Offset the position
        QPointF pos = event->scenePos();
        qreal angle = QLineF(seg.p1(), seg.p2()).angle();
        // When the wire is horizontal move the label up
        if (seg.isHorizontal()) {
            pos.setY(seg.p1().y() - _settings.gridSize / 2);
        }
        // When the wire is vertical move the label to the right
        else if (seg.isVertical()) {
            pos.setX(seg.p1().x() + _settings.gridSize / 2);
        }
        // When the wire is diagonal with a positive slope move it up and to the left
        else if ((angle > 0 and angle < 90) or (angle > 180 and angle < 360)) {
            QPointF point = Utils::pointOnLineClosestToPoint(seg.p1(), seg.p2(), pos);
            pos.setX(point.x() - _settings.gridSize / 2 - net()->label()->textRect().width());
            pos.setY(point.y() - _settings.gridSize / 2);
        }
        // When the wire is diagonal with a negative slope move it up and to the right
        else {
            QPointF point = Utils::pointOnLineClosestToPoint(seg.p1(), seg.p2(), pos);
            pos.setX(point.x() + _settings.gridSize / 2);
            pos.setY(point.y() - _settings.gridSize / 2);
        }
        net()->label()->setParentItem(this);
        net()->label()->setPos(pos - Wire::pos());
    }
}

std::shared_ptr<WireNet> Wire::net()
{
    return _net;
}

void Wire::setNet(const std::shared_ptr<WireNet>& wirenet)
{
    _net = wirenet;
}

bool Wire::movingWirePoint() const
{
    if (_pointToMoveIndex >= 0 or _lineSegmentToMoveIndex >= 0) {
        return true;
    } else {
        return false;
    }
}

void Wire::about_to_change()
{
    prepareGeometryChange();
}

void Wire::has_changed()
{
    calculateBoundingRect();
}

void Wire::add_segment(int index)
{
    if (index == 0) {
        _lineSegmentToMoveIndex++;
    }
    wire::add_segment(index);
}
