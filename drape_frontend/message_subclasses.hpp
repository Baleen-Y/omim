#pragma once

#include "drape_frontend/circles_pack_shape.hpp"
#include "drape_frontend/color_constants.hpp"
#include "drape_frontend/custom_symbol.hpp"
#include "drape_frontend/drape_api.hpp"
#include "drape_frontend/drape_api_builder.hpp"
#include "drape_frontend/gps_track_point.hpp"
#include "drape_frontend/gui/layer_render.hpp"
#include "drape_frontend/gui/skin.hpp"
#include "drape_frontend/message.hpp"
#include "drape_frontend/my_position.hpp"
#include "drape_frontend/overlay_batcher.hpp"
#include "drape_frontend/postprocess_renderer.hpp"
#include "drape_frontend/route_builder.hpp"
#include "drape_frontend/selection_shape.hpp"
#include "drape_frontend/tile_utils.hpp"
#include "drape_frontend/traffic_generator.hpp"
#include "drape_frontend/user_mark_shapes.hpp"
#include "drape_frontend/user_marks_provider.hpp"

#include "drape/glstate.hpp"
#include "drape/pointers.hpp"
#include "drape/render_bucket.hpp"
#include "drape/viewport.hpp"

#include "geometry/polyline2d.hpp"
#include "geometry/rect2d.hpp"
#include "geometry/screenbase.hpp"
#include "geometry/triangle2d.hpp"

#include "platform/location.hpp"

#include "std/condition_variable.hpp"
#include "std/function.hpp"
#include "std/set.hpp"
#include "std/shared_ptr.hpp"
#include "std/utility.hpp"

namespace df
{

class BaseBlockingMessage : public Message
{
public:
  struct Blocker
  {
    void Wait()
    {
      unique_lock<mutex> lock(m_lock);
      m_signal.wait(lock, [this]{return !m_blocked;} );
    }

  private:
    friend class BaseBlockingMessage;

    void Signal()
    {
      lock_guard<mutex> lock(m_lock);
      m_blocked = false;
      m_signal.notify_one();
    }

  private:
    mutex m_lock;
    condition_variable m_signal;
    bool m_blocked = true;
  };

  BaseBlockingMessage(Blocker & blocker)
    : m_blocker(blocker)
  {
  }

  ~BaseBlockingMessage()
  {
    m_blocker.Signal();
  }

private:
  Blocker & m_blocker;
};

class BaseTileMessage : public Message
{
public:
  BaseTileMessage(TileKey const & key)
    : m_tileKey(key) {}

  TileKey const & GetKey() const { return m_tileKey; }

private:
  TileKey m_tileKey;
};

class FinishReadingMessage : public Message
{
public:
  FinishReadingMessage() = default;

  Type GetType() const override { return Message::FinishReading; }
};

class FinishTileReadMessage : public Message
{
public:
  template<typename T> FinishTileReadMessage(T && tiles)
    : m_tiles(forward<T>(tiles))
  {}

  Type GetType() const override { return Message::FinishTileRead; }

  TTilesCollection const & GetTiles() const { return m_tiles; }
  TTilesCollection && MoveTiles() { return move(m_tiles); }

private:
  TTilesCollection m_tiles;
};

class FlushRenderBucketMessage : public BaseTileMessage
{
public:
  FlushRenderBucketMessage(TileKey const & key, dp::GLState const & state, drape_ptr<dp::RenderBucket> && buffer)
    : BaseTileMessage(key)
    , m_state(state)
    , m_buffer(move(buffer))
  {}

  Type GetType() const override { return Message::FlushTile; }
  bool IsGLContextDependent() const override { return true; }

  dp::GLState const & GetState() const { return m_state; }
  drape_ptr<dp::RenderBucket> && AcceptBuffer() { return move(m_buffer); }

private:
  dp::GLState m_state;
  drape_ptr<dp::RenderBucket> m_buffer;
};

class FlushOverlaysMessage : public Message
{
public:
  FlushOverlaysMessage(TOverlaysRenderData && data) : m_data(move(data)) {}

  Type GetType() const override { return Message::FlushOverlays; }
  bool IsGLContextDependent() const override { return true; }

  TOverlaysRenderData && AcceptRenderData() { return move(m_data); }

private:
  TOverlaysRenderData m_data;
};

class InvalidateRectMessage : public Message
{
public:
  InvalidateRectMessage(m2::RectD const & rect)
    : m_rect(rect) {}

  Type GetType() const override { return Message::InvalidateRect; }

  m2::RectD const & GetRect() const { return m_rect; }

private:
  m2::RectD m_rect;
};

class UpdateReadManagerMessage : public Message
{
public:
  UpdateReadManagerMessage(){}

  Type GetType() const override { return Message::UpdateReadManager; }
};

class InvalidateReadManagerRectMessage : public BaseBlockingMessage
{
public:
  InvalidateReadManagerRectMessage(Blocker & blocker, TTilesCollection const & tiles)
    : BaseBlockingMessage(blocker)
    , m_tiles(tiles)
    , m_needInvalidateAll(false)
  {}

  InvalidateReadManagerRectMessage(Blocker & blocker)
    : BaseBlockingMessage(blocker)
    , m_needInvalidateAll(true)
  {}

  Type GetType() const override { return Message::InvalidateReadManagerRect; }

  TTilesCollection const & GetTilesForInvalidate() const { return m_tiles; }
  bool NeedInvalidateAll() const { return m_needInvalidateAll; }

private:
  TTilesCollection m_tiles;
  bool m_needInvalidateAll;
};

class BaseUserMarkLayerMessage : public Message
{
public:
  BaseUserMarkLayerMessage(size_t layerId)
    : m_layerId(layerId)
  {}

  size_t GetLayerId() const { return m_layerId; }

private:
  size_t m_layerId;
};

class ClearUserMarkLayerMessage : public BaseUserMarkLayerMessage
{
public:
  ClearUserMarkLayerMessage(size_t layerId)
    : BaseUserMarkLayerMessage(layerId) {}

  Type GetType() const override { return Message::ClearUserMarkLayer; }
};

class ChangeUserMarkLayerVisibilityMessage : public BaseUserMarkLayerMessage
{
public:
  ChangeUserMarkLayerVisibilityMessage(size_t layerId, bool isVisible)
    : BaseUserMarkLayerMessage(layerId)
    , m_isVisible(isVisible) {}

  Type GetType() const override { return Message::ChangeUserMarkLayerVisibility; }

  bool IsVisible() const { return m_isVisible; }

private:
  bool m_isVisible;
};

class UpdateUserMarkLayerMessage : public BaseUserMarkLayerMessage
{
public:
  UpdateUserMarkLayerMessage(size_t layerId, UserMarksProvider * provider)
    : BaseUserMarkLayerMessage(layerId)
    , m_provider(provider)
  {
    m_provider->IncrementCounter();
  }

  ~UpdateUserMarkLayerMessage() override
  {
    ASSERT(m_inProcess == false, ());
    m_provider->DecrementCounter();
    if (m_provider->IsPendingOnDelete() && m_provider->CanBeDeleted())
      delete m_provider;
  }

  Type GetType() const override { return Message::UpdateUserMarkLayer; }

  UserMarksProvider const * StartProcess()
  {
    m_provider->BeginRead();
#ifdef DEBUG
    m_inProcess = true;
#endif
    return m_provider;
  }

  void EndProcess()
  {
#ifdef DEBUG
    m_inProcess = false;
#endif
    m_provider->EndRead();
  }

private:
  UserMarksProvider * m_provider;
#ifdef DEBUG
  bool m_inProcess;
#endif
};

class FlushUserMarksMessage : public BaseUserMarkLayerMessage
{
public:
  FlushUserMarksMessage(size_t layerId, TUserMarkShapes && shapes)
    : BaseUserMarkLayerMessage(layerId)
    , m_shapes(move(shapes))
  {}

  Type GetType() const override { return Message::FlushUserMarks; }
  bool IsGLContextDependent() const override { return true; }

  TUserMarkShapes & GetShapes() { return m_shapes; }

private:
  TUserMarkShapes m_shapes;
};

class GuiLayerRecachedMessage : public Message
{
public:
  GuiLayerRecachedMessage(drape_ptr<gui::LayerRenderer> && renderer, bool needResetOldGui)
    : m_renderer(move(renderer))
    , m_needResetOldGui(needResetOldGui)
  {}

  Type GetType() const override { return Message::GuiLayerRecached; }
  bool IsGLContextDependent() const override { return true; }

  drape_ptr<gui::LayerRenderer> && AcceptRenderer() { return move(m_renderer); }
  bool NeedResetOldGui() const { return m_needResetOldGui; }

private:
  drape_ptr<gui::LayerRenderer> m_renderer;
  bool const m_needResetOldGui;
};

class GuiRecacheMessage : public Message
{
public:
  GuiRecacheMessage(gui::TWidgetsInitInfo const & initInfo, bool needResetOldGui)
    : m_initInfo(initInfo)
    , m_needResetOldGui(needResetOldGui)
  {}

  Type GetType() const override { return Message::GuiRecache;}
  bool IsGLContextDependent() const override { return true; }

  gui::TWidgetsInitInfo const & GetInitInfo() const { return m_initInfo; }
  bool NeedResetOldGui() const { return m_needResetOldGui; }

private:
  gui::TWidgetsInitInfo m_initInfo;
  bool const m_needResetOldGui;
};

class MapShapesRecacheMessage : public Message
{
public:
  MapShapesRecacheMessage() = default;

  Type GetType() const override { return Message::MapShapesRecache; }
  bool IsGLContextDependent() const override { return true; }
};

class GuiLayerLayoutMessage : public Message
{
public:
  GuiLayerLayoutMessage(gui::TWidgetsLayoutInfo const & info)
    : m_layoutInfo(info)
  {}

  Type GetType() const override { return GuiLayerLayout; }
  bool IsGLContextDependent() const override { return true; }

  gui::TWidgetsLayoutInfo const & GetLayoutInfo() const { return m_layoutInfo; }
  gui::TWidgetsLayoutInfo AcceptLayoutInfo() { return move(m_layoutInfo); }

private:
  gui::TWidgetsLayoutInfo m_layoutInfo;
};

class ShowChoosePositionMarkMessage : public Message
{
public:
  ShowChoosePositionMarkMessage() = default;
  Type GetType() const override { return Message::ShowChoosePositionMark; }
};

class SetKineticScrollEnabledMessage : public Message
{
public:
  SetKineticScrollEnabledMessage(bool enabled)
    : m_enabled(enabled)
  {}

  Type GetType() const override { return Message::SetKineticScrollEnabled; }

  bool IsEnabled() const { return m_enabled; }

private:
  bool m_enabled;
};

class SetAddNewPlaceModeMessage : public Message
{
public:
  SetAddNewPlaceModeMessage(bool enable, vector<m2::TriangleD> && boundArea, bool enableKineticScroll,
                            bool hasPosition, m2::PointD const & position)
    : m_enable(enable)
    , m_boundArea(move(boundArea))
    , m_enableKineticScroll(enableKineticScroll)
    , m_hasPosition(hasPosition)
    , m_position(position)
  {}

  Type GetType() const override { return Message::SetAddNewPlaceMode; }

  vector<m2::TriangleD> && AcceptBoundArea() { return move(m_boundArea); }
  bool IsEnabled() const { return m_enable; }
  bool IsKineticScrollEnabled() const { return m_enableKineticScroll; }
  bool HasPosition() const { return m_hasPosition; }
  m2::PointD const & GetPosition() const { return m_position; }

private:
  bool m_enable;
  vector<m2::TriangleD> m_boundArea;
  bool m_enableKineticScroll;
  bool m_hasPosition;
  m2::PointD m_position;
};

class BlockTapEventsMessage : public Message
{
public:
  BlockTapEventsMessage(bool block)
    : m_needBlock(block)
  {}

  Type GetType() const override { return Message::BlockTapEvents; }

  bool NeedBlock() const { return m_needBlock; }

private:
  bool const m_needBlock;
};

class MapShapesMessage : public Message
{
public:
  MapShapesMessage(drape_ptr<MyPosition> && shape, drape_ptr<SelectionShape> && selection)
    : m_shape(move(shape))
    , m_selection(move(selection))
  {}

  Type GetType() const override { return Message::MapShapes; }
  bool IsGLContextDependent() const override { return true; }

  drape_ptr<MyPosition> && AcceptShape() { return move(m_shape); }
  drape_ptr<SelectionShape> AcceptSelection() { return move(m_selection); }

private:
  drape_ptr<MyPosition> m_shape;
  drape_ptr<SelectionShape> m_selection;
};

class ChangeMyPositionModeMessage : public Message
{
public:
  enum EChangeType
  {
    SwitchNextMode,
    LoseLocation,
    StopFollowing
  };

  explicit ChangeMyPositionModeMessage(EChangeType changeType)
    : m_changeType(changeType)
  {}

  EChangeType GetChangeType() const { return m_changeType; }
  Type GetType() const override { return Message::ChangeMyPostitionMode; }

private:
  EChangeType const m_changeType;
};

class CompassInfoMessage : public Message
{
public:
  CompassInfoMessage(location::CompassInfo const & info)
    : m_info(info)
  {}

  Type GetType() const override { return Message::CompassInfo; }

  location::CompassInfo const & GetInfo() const { return m_info; }

private:
  location::CompassInfo const m_info;
};

class GpsInfoMessage : public Message
{
public:
  GpsInfoMessage(location::GpsInfo const & info, bool isNavigable,
                 location::RouteMatchingInfo const & routeInfo)
    : m_info(info)
    , m_isNavigable(isNavigable)
    , m_routeInfo(routeInfo)
  {}

  Type GetType() const override { return Message::GpsInfo; }

  location::GpsInfo const & GetInfo() const { return m_info; }
  bool IsNavigable() const { return m_isNavigable; }
  location::RouteMatchingInfo const & GetRouteInfo() const { return m_routeInfo; }

private:
  location::GpsInfo const m_info;
  bool const m_isNavigable;
  location::RouteMatchingInfo const m_routeInfo;
};

class FindVisiblePOIMessage : public BaseBlockingMessage
{
public:
  FindVisiblePOIMessage(Blocker & blocker, m2::PointD const & glbPt, FeatureID & featureID)
    : BaseBlockingMessage(blocker)
    , m_pt(glbPt)
    , m_featureID(featureID)
  {}

  Type GetType() const override { return FindVisiblePOI; }

  m2::PointD const & GetPoint() const { return m_pt; }
  void SetFeatureID(FeatureID const & id)
  {
    m_featureID = id;
  }

private:
  m2::PointD m_pt;
  FeatureID & m_featureID;
};

class SelectObjectMessage : public Message
{
public:
  struct DismissTag {};

  SelectObjectMessage(DismissTag)
    : m_selected(SelectionShape::OBJECT_EMPTY)
    , m_glbPoint(m2::PointD::Zero())
    , m_isAnim(false)
    , m_isDismiss(true)
  {}

  SelectObjectMessage(SelectionShape::ESelectedObject selectedObject, m2::PointD const & glbPoint, FeatureID const & featureID,  bool isAnim)
    : m_selected(selectedObject)
    , m_glbPoint(glbPoint)
    , m_featureID(featureID)
    , m_isAnim(isAnim)
    , m_isDismiss(false)
  {}

  Type GetType() const override { return SelectObject; }
  bool IsGLContextDependent() const override { return true; }

  m2::PointD const & GetPosition() const { return m_glbPoint; }
  SelectionShape::ESelectedObject GetSelectedObject() const { return m_selected; }
  FeatureID const & GetFeatureID() const { return m_featureID; }
  bool IsAnim() const { return m_isAnim; }
  bool IsDismiss() const { return m_isDismiss; }

private:
  SelectionShape::ESelectedObject m_selected;
  m2::PointD m_glbPoint;
  FeatureID m_featureID;
  bool m_isAnim;
  bool m_isDismiss;
};

class GetSelectedObjectMessage : public BaseBlockingMessage
{
public:
  GetSelectedObjectMessage(Blocker & blocker, SelectionShape::ESelectedObject & object)
    : BaseBlockingMessage(blocker)
    , m_object(object)
  {}

  Type GetType() const override { return GetSelectedObject; }

  void SetSelectedObject(SelectionShape::ESelectedObject const & object)
  {
    m_object = object;
  }

private:
  SelectionShape::ESelectedObject & m_object;
};

class GetMyPositionMessage : public BaseBlockingMessage
{
public:
  GetMyPositionMessage(Blocker & blocker, bool & hasPosition, m2::PointD & myPosition)
    : BaseBlockingMessage(blocker)
    , m_myPosition(myPosition)
    , m_hasPosition(hasPosition)
  {}

  Type GetType() const override { return GetMyPosition; }

  void SetMyPosition(bool hasPosition, m2::PointD const & myPosition)
  {
    m_hasPosition = hasPosition;
    m_myPosition = myPosition;
  }

private:
  m2::PointD & m_myPosition;
  bool & m_hasPosition;
};

class AddRouteSegmentMessage : public Message
{
public:
  AddRouteSegmentMessage(dp::DrapeID segmentId, drape_ptr<RouteSegment> && segment)
    : AddRouteSegmentMessage(segmentId, std::move(segment), -1 /* invalid recache id */)
  {}

  AddRouteSegmentMessage(dp::DrapeID segmentId, drape_ptr<RouteSegment> && segment,
                         int recacheId)
    : m_segmentId(segmentId)
    , m_segment(std::move(segment))
    , m_recacheId(recacheId)
  {}

  Type GetType() const override { return Message::AddRouteSegment; }

  dp::DrapeID GetSegmentId() const { return m_segmentId; };
  drape_ptr<RouteSegment> && GetRouteSegment() { return std::move(m_segment); }
  int GetRecacheId() const { return m_recacheId; }

private:
  dp::DrapeID m_segmentId;
  drape_ptr<RouteSegment> m_segment;
  int const m_recacheId;
};

class CacheRouteArrowsMessage : public Message
{
public:
  CacheRouteArrowsMessage(dp::DrapeID segmentId, std::vector<ArrowBorders> const & borders)
    : CacheRouteArrowsMessage(segmentId, borders, -1 /* invalid recache id */)
  {}

  CacheRouteArrowsMessage(dp::DrapeID segmentId, std::vector<ArrowBorders> const & borders,
                          int recacheId)
    : m_segmentId(segmentId)
    , m_borders(borders)
    , m_recacheId(recacheId)
  {}

  Type GetType() const override { return Message::CacheRouteArrows; }
  dp::DrapeID GetSegmentId() const { return m_segmentId; }
  std::vector<ArrowBorders> const & GetBorders() const { return m_borders; }
  int GetRecacheId() const { return m_recacheId; }

private:
  dp::DrapeID m_segmentId;
  std::vector<ArrowBorders> m_borders;
  int const m_recacheId;
};

class RemoveRouteSegmentMessage : public Message
{
public:
  RemoveRouteSegmentMessage(dp::DrapeID segmentId, bool deactivateFollowing)
    : m_segmentId(segmentId)
    , m_deactivateFollowing(deactivateFollowing)
  {}

  Type GetType() const override { return Message::RemoveRouteSegment; }

  dp::DrapeID GetSegmentId() const { return m_segmentId; }
  bool NeedDeactivateFollowing() const { return m_deactivateFollowing; }

private:
  dp::DrapeID m_segmentId;
  bool m_deactivateFollowing;
};

class FlushRouteMessage : public Message
{
public:
  FlushRouteMessage(drape_ptr<RouteData> && routeData)
    : m_routeData(std::move(routeData))
  {}

  Type GetType() const override { return Message::FlushRoute; }

  bool IsGLContextDependent() const override { return true; }
  drape_ptr<RouteData> && AcceptRouteData() { return std::move(m_routeData); }

private:
  drape_ptr<RouteData> m_routeData;
};

class FlushRouteArrowsMessage : public Message
{
public:
  FlushRouteArrowsMessage(drape_ptr<RouteArrowsData> && routeArrowsData)
    : m_routeArrowsData(std::move(routeArrowsData))
  {}

  Type GetType() const override { return Message::FlushRouteArrows; }

  drape_ptr<RouteArrowsData> && AcceptRouteArrowsData() { return std::move(m_routeArrowsData); }

private:
  drape_ptr<RouteArrowsData> m_routeArrowsData;
};

class AddRoutePreviewSegmentMessage : public Message
{
public:
  AddRoutePreviewSegmentMessage(dp::DrapeID segmentId, m2::PointD const & startPt,
                                m2::PointD const & finishPt)
    : m_segmentId(segmentId)
    , m_startPoint(startPt)
    , m_finishPoint(finishPt)
  {}

  Type GetType() const override { return Message::AddRoutePreviewSegment; }

  dp::DrapeID GetSegmentId() const { return m_segmentId; };
  m2::PointD const & GetStartPoint() const { return m_startPoint; }
  m2::PointD const & GetFinishPoint() const { return m_finishPoint; }

private:
  dp::DrapeID m_segmentId;
  m2::PointD m_startPoint;
  m2::PointD m_finishPoint;
};

class RemoveRoutePreviewSegmentMessage : public Message
{
public:
  RemoveRoutePreviewSegmentMessage()
    : m_needRemoveAll(true)
  {}

  explicit RemoveRoutePreviewSegmentMessage(dp::DrapeID segmentId)
    : m_segmentId(segmentId)
    , m_needRemoveAll(false)
  {}

  Type GetType() const override { return Message::RemoveRoutePreviewSegment; }

  dp::DrapeID GetSegmentId() const { return m_segmentId; }
  bool NeedRemoveAll() const { return m_needRemoveAll; }

private:
  dp::DrapeID m_segmentId;
  bool m_needRemoveAll;
};

class SetRouteSegmentVisibilityMessage : public Message
{
public:
  SetRouteSegmentVisibilityMessage(dp::DrapeID segmentId, bool isVisible)
    : m_segmentId(segmentId)
    , m_isVisible(isVisible)
  {}

  Type GetType() const override { return Message::SetRouteSegmentVisibility; }

  dp::DrapeID GetSegmentId() const { return m_segmentId; }
  bool IsVisible() const { return m_isVisible; }

private:
  dp::DrapeID m_segmentId;
  bool m_isVisible;
};

class UpdateMapStyleMessage : public BaseBlockingMessage
{
public:
  UpdateMapStyleMessage(Blocker & blocker)
    : BaseBlockingMessage(blocker)
  {}

  Type GetType() const override { return Message::UpdateMapStyle; }
};

class FollowRouteMessage : public Message
{
public:
  FollowRouteMessage(int preferredZoomLevel, int preferredZoomLevelIn3d, bool enableAutoZoom)
    : m_preferredZoomLevel(preferredZoomLevel)
    , m_preferredZoomLevelIn3d(preferredZoomLevelIn3d)
    , m_enableAutoZoom(enableAutoZoom)
  {}

  Type GetType() const override { return Message::FollowRoute; }

  int GetPreferredZoomLevel() const { return m_preferredZoomLevel; }
  int GetPreferredZoomLevelIn3d() const { return m_preferredZoomLevelIn3d; }
  bool EnableAutoZoom() const { return m_enableAutoZoom; }

private:
  int const m_preferredZoomLevel;
  int const m_preferredZoomLevelIn3d;
  bool const m_enableAutoZoom;
};

class SwitchMapStyleMessage : public BaseBlockingMessage
{
public:
  SwitchMapStyleMessage(Blocker & blocker)
    : BaseBlockingMessage(blocker)
  {}

  Type GetType() const override { return Message::SwitchMapStyle; }
};

class InvalidateMessage : public Message
{
public:
  InvalidateMessage(){}

  Type GetType() const override { return Message::Invalidate; }
};

class RecoverGLResourcesMessage : public Message
{
public:
  RecoverGLResourcesMessage(){}

  Type GetType() const override { return Message::RecoverGLResources; }
  bool IsGLContextDependent() const override { return true; }
};

class SetVisibleViewportMessage : public Message
{
public:
  SetVisibleViewportMessage(m2::RectD const & rect)
    : m_rect(rect)
  {}

  Type GetType() const override { return Message::SetVisibleViewport;  }

  m2::RectD const &  GetRect() const { return m_rect; }

private:
  m2::RectD m_rect;
};

class DeactivateRouteFollowingMessage : public Message
{
public:
  DeactivateRouteFollowingMessage(){}

  Type GetType() const override { return Message::DeactivateRouteFollowing; }
};

class Allow3dModeMessage : public Message
{
public:
  Allow3dModeMessage(bool allowPerspective, bool allow3dBuildings)
    : m_allowPerspective(allowPerspective)
    , m_allow3dBuildings(allow3dBuildings)
  {}

  Type GetType() const override { return Message::Allow3dMode; }

  bool AllowPerspective() const { return m_allowPerspective; }
  bool Allow3dBuildings() const { return m_allow3dBuildings; }

private:
  bool const m_allowPerspective;
  bool const m_allow3dBuildings;
};

class AllowAutoZoomMessage : public Message
{
public:
  AllowAutoZoomMessage(bool allowAutoZoom)
    : m_allowAutoZoom(allowAutoZoom)
  {}

  Type GetType() const override { return Message::AllowAutoZoom; }

  bool AllowAutoZoom() const { return m_allowAutoZoom; }

private:
  bool const m_allowAutoZoom;
};

class Allow3dBuildingsMessage : public Message
{
public:
  Allow3dBuildingsMessage(bool allow3dBuildings)
    : m_allow3dBuildings(allow3dBuildings)
  {}

  Type GetType() const override { return Message::Allow3dBuildings; }

  bool Allow3dBuildings() const { return m_allow3dBuildings; }

private:
  bool const m_allow3dBuildings;
};

class EnablePerspectiveMessage : public Message
{
public:
  EnablePerspectiveMessage() = default;

  Type GetType() const override { return Message::EnablePerspective; }
};

class CacheCirclesPackMessage : public Message
{
public:
  enum Destination
  {
    GpsTrack,
    RoutePreview
  };

  CacheCirclesPackMessage(uint32_t pointsCount, Destination dest)
    : m_pointsCount(pointsCount)
    , m_destination(dest)
  {}

  Type GetType() const override { return Message::CacheCirclesPack; }

  uint32_t GetPointsCount() const { return m_pointsCount; }
  Destination GetDestination() const { return m_destination; }

private:
  uint32_t m_pointsCount;
  Destination m_destination;
};

class FlushCirclesPackMessage : public Message
{
public:

  FlushCirclesPackMessage(drape_ptr<CirclesPackRenderData> && renderData,
                          CacheCirclesPackMessage::Destination dest)
    : m_renderData(std::move(renderData))
    , m_destination(dest)
  {}

  Type GetType() const override { return Message::FlushCirclesPack; }
  bool IsGLContextDependent() const override { return true; }

  drape_ptr<CirclesPackRenderData> && AcceptRenderData() { return std::move(m_renderData); }
  CacheCirclesPackMessage::Destination GetDestination() const { return m_destination; }

private:
  drape_ptr<CirclesPackRenderData> m_renderData;
  CacheCirclesPackMessage::Destination m_destination;
};

class UpdateGpsTrackPointsMessage : public Message
{
public:
  UpdateGpsTrackPointsMessage(std::vector<GpsTrackPoint> && toAdd,
                              std::vector<uint32_t> && toRemove)
    : m_pointsToAdd(std::move(toAdd))
    , m_pointsToRemove(std::move(toRemove))
  {}

  Type GetType() const override { return Message::UpdateGpsTrackPoints; }

  std::vector<GpsTrackPoint> const & GetPointsToAdd() { return m_pointsToAdd; }
  std::vector<uint32_t> const & GetPointsToRemove() { return m_pointsToRemove; }

private:
  std::vector<GpsTrackPoint> m_pointsToAdd;
  std::vector<uint32_t> m_pointsToRemove;
};

class ClearGpsTrackPointsMessage : public Message
{
public:
  ClearGpsTrackPointsMessage() = default;

  Type GetType() const override { return Message::ClearGpsTrackPoints; }
};

class SetTimeInBackgroundMessage : public Message
{
public:
  explicit SetTimeInBackgroundMessage(double time)
    : m_time(time)
  {}

  Type GetType() const override { return Message::SetTimeInBackground; }

  double GetTime() const { return m_time; }

private:
  double m_time;
};

class SetDisplacementModeMessage : public Message
{
public:
  explicit SetDisplacementModeMessage(int mode)
    : m_mode(mode)
  {}

  Type GetType() const override { return Message::SetDisplacementMode; }

  int GetMode() const { return m_mode; }

private:
  int m_mode;
};

class RequestSymbolsSizeMessage : public Message
{
public:
  using TRequestSymbolsSizeCallback = function<void(vector<m2::PointF> const &)>;

  RequestSymbolsSizeMessage(vector<string> const & symbols,
                            TRequestSymbolsSizeCallback const & callback)
    : m_symbols(symbols)
    , m_callback(callback)
  {}

  Type GetType() const override { return Message::RequestSymbolsSize; }

  vector<string> const & GetSymbols() const { return m_symbols; }

  void InvokeCallback(vector<m2::PointF> const & sizes)
  {
    if (m_callback != nullptr)
      m_callback(sizes);
  }

private:
  vector<string> m_symbols;
  TRequestSymbolsSizeCallback m_callback;
};

class EnableTrafficMessage : public Message
{
public:
  explicit EnableTrafficMessage(bool trafficEnabled)
    : m_trafficEnabled(trafficEnabled)
  {}

  Type GetType() const override { return Message::EnableTraffic; }

  bool IsTrafficEnabled() const { return m_trafficEnabled; }

private:
  bool const m_trafficEnabled;
};

class FlushTrafficGeometryMessage : public BaseTileMessage
{
public:
  FlushTrafficGeometryMessage(TileKey const & tileKey, TrafficSegmentsGeometry && segments)
    : BaseTileMessage(tileKey)
    , m_segments(move(segments))
  {}

  Type GetType() const override { return Message::FlushTrafficGeometry; }

  TrafficSegmentsGeometry & GetSegments() { return m_segments; }

private:
  TrafficSegmentsGeometry m_segments;
};

class RegenerateTrafficMessage : public Message
{
public:
  Type GetType() const override { return Message::RegenerateTraffic; }
};

class UpdateTrafficMessage : public Message
{
public:
  explicit UpdateTrafficMessage(TrafficSegmentsColoring && segmentsColoring)
    : m_segmentsColoring(move(segmentsColoring))
  {}

  Type GetType() const override { return Message::UpdateTraffic; }

  TrafficSegmentsColoring & GetSegmentsColoring() { return m_segmentsColoring; }

private:
  TrafficSegmentsColoring m_segmentsColoring;
};

class FlushTrafficDataMessage : public Message
{
public:
  explicit FlushTrafficDataMessage(TrafficRenderData && trafficData)
    : m_trafficData(move(trafficData))
  {}

  Type GetType() const override { return Message::FlushTrafficData; }
  bool IsGLContextDependent() const override { return true; }

  TrafficRenderData && AcceptTrafficData() { return move(m_trafficData); }

private:
  TrafficRenderData m_trafficData;
};

class ClearTrafficDataMessage : public Message
{
public:
  explicit ClearTrafficDataMessage(MwmSet::MwmId const & mwmId)
    : m_mwmId(mwmId)
  {}

  Type GetType() const override { return Message::ClearTrafficData; }

  MwmSet::MwmId const & GetMwmId() { return m_mwmId; }

private:
  MwmSet::MwmId m_mwmId;
};

class SetSimplifiedTrafficColorsMessage : public Message
{
public:
  SetSimplifiedTrafficColorsMessage(bool isSimplified)
    : m_isSimplified(isSimplified)
  {}

  Type GetType() const override { return Message::SetSimplifiedTrafficColors; }

  bool IsSimplified() const { return m_isSimplified; }

private:
  bool const m_isSimplified;
};

class DrapeApiAddLinesMessage : public Message
{
public:
  explicit DrapeApiAddLinesMessage(DrapeApi::TLines const & lines)
    : m_lines(lines)
  {}

  Type GetType() const override { return Message::DrapeApiAddLines; }

  DrapeApi::TLines const & GetLines() const { return m_lines; }

private:
  DrapeApi::TLines m_lines;
};

class DrapeApiRemoveMessage : public Message
{
public:
  explicit DrapeApiRemoveMessage(string const & id, bool removeAll = false)
    : m_id(id)
    , m_removeAll(removeAll)
  {}

  Type GetType() const override { return Message::DrapeApiRemove; }

  string const & GetId() const { return m_id; }
  bool NeedRemoveAll() const { return m_removeAll; }

private:
  string m_id;
  bool m_removeAll;
};

class DrapeApiFlushMessage : public Message
{
public:
  using TProperties = vector<drape_ptr<DrapeApiRenderProperty>>;

  explicit DrapeApiFlushMessage(TProperties && properties)
    : m_properties(move(properties))
  {}

  Type GetType() const override { return Message::DrapeApiFlush; }

  TProperties && AcceptProperties() { return move(m_properties); }

private:
  TProperties m_properties;
};

class AddCustomSymbolsMessage : public Message
{
public:
  explicit AddCustomSymbolsMessage(CustomSymbols && symbols)
    : m_symbols(std::move(symbols))
  {}

  Type GetType() const override { return Message::AddCustomSymbols; }

  CustomSymbols && AcceptSymbols() { return std::move(m_symbols); }

private:
  CustomSymbols m_symbols;
};

class RemoveCustomSymbolsMessage : public Message
{
public:
  RemoveCustomSymbolsMessage() = default;
  explicit RemoveCustomSymbolsMessage(MwmSet::MwmId const & mwmId)
    : m_mwmId(mwmId), m_removeAll(false)
  {}

  Type GetType() const override { return Message::RemoveCustomSymbols; }
  bool NeedRemoveAll() const { return m_removeAll; }
  MwmSet::MwmId const & GetMwmId() const { return m_mwmId; }

private:
  MwmSet::MwmId m_mwmId;
  bool m_removeAll = true;
};

class UpdateCustomSymbolsMessage : public Message
{
public:
  explicit UpdateCustomSymbolsMessage(std::vector<FeatureID> && symbolsFeatures)
    : m_symbolsFeatures(std::move(symbolsFeatures))
  {}

  Type GetType() const override { return Message::UpdateCustomSymbols; }

  std::vector<FeatureID> && AcceptSymbolsFeatures() { return std::move(m_symbolsFeatures); }

private:
  std::vector<FeatureID> m_symbolsFeatures;
};

class SetPostprocessStaticTexturesMessage : public Message
{
public:
  explicit SetPostprocessStaticTexturesMessage(drape_ptr<PostprocessStaticTextures> && textures)
    : m_textures(std::move(textures))
  {}

  Type GetType() const override { return Message::SetPostprocessStaticTextures; }

  drape_ptr<PostprocessStaticTextures> && AcceptTextures() { return std::move(m_textures); }

private:
  drape_ptr<PostprocessStaticTextures> m_textures;
};

class SetPosteffectEnabledMessage : public Message
{
public:
  SetPosteffectEnabledMessage(PostprocessRenderer::Effect effect, bool enabled)
    : m_effect(effect)
    , m_enabled(enabled)
  {}

  Type GetType() const override { return Message::SetPosteffectEnabled; }
  PostprocessRenderer::Effect GetEffect() const { return m_effect; }
  bool IsEnabled() const { return m_enabled; }

private:
  PostprocessRenderer::Effect const m_effect;
  bool const m_enabled;
};

class RunFirstLaunchAnimationMessage : public Message
{
public:
  Type GetType() const override { return Message::RunFirstLaunchAnimation; }
};

class UpdateMetalinesMessage : public Message
{
public:
  Type GetType() const override { return Message::UpdateMetalines; }
};
}  // namespace df
