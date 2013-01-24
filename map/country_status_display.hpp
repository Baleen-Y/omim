#pragma once

#include "../storage/storage.hpp"

#include "../gui/element.hpp"
#include "../gui/button.hpp"
#include "../gui/text_view.hpp"

#include "../std/shared_ptr.hpp"


/// This class is a composite GUI element to display
/// an on-screen GUI for the country, which is not downloaded yet.
class CountryStatusDisplay : public gui::Element
{
private:

  /// Storage-related members and methods
  /// @{
  /// connection to the Storage for notifications
  unsigned m_slotID;
  storage::Storage * m_storage;
  /// notification callback upon country status change
  void CountryStatusChanged(storage::TIndex const &);
  /// notification callback upon country downloading progress
  void CountryProgress(storage::TIndex const &, pair<int64_t, int64_t> const & progress);
  /// @}

  void UpdateStatusAndProgress();

  /// download button
  shared_ptr<gui::Button> m_downloadButton;
  /// country status message
  shared_ptr<gui::TextView> m_statusMsg;
  /// current map name, "Province" part of the fullName
  string m_mapName;
  /// current map group name, "Country" part of the fullName
  string m_mapGroupName;
  /// current country status
  storage::TStatus m_countryStatus;
  /// index of the country in Storage
  storage::TIndex m_countryIdx;
  /// downloading progress of the country
  pair<int64_t, int64_t> m_countryProgress;

  bool m_notEnoughSpace;

  /// bounding rects
  mutable vector<m2::AnyRectD> m_boundRects;

  /// caching resources for fast rendering.
  void cache();
  void purge();

  string const displayName() const;

  template <class T1, class T2>
  void SetStatusMessage(string const & msgID, T1 const * t1 = 0, T2 const * t2 = 0);

public:

  struct Params : public gui::Element::Params
  {
    storage::Storage * m_storage;
  };

  CountryStatusDisplay(Params const & p);
  ~CountryStatusDisplay();

  /// start country download
  void downloadCountry();
  /// set download button listener
  void setDownloadListener(gui::Button::TOnClickListener const & l);
  /// set current country name
  void setCountryIndex(storage::TIndex const & idx);
  /// reposition element
  void setPivot(m2::PointD const & pv);
  /// attach element to controller.
  void setController(gui::Controller *controller);
  /// render element
  void draw(graphics::OverlayRenderer * r, math::Matrix<double, 3, 3> const & m) const;
  /// get bounding rects
  vector<m2::AnyRectD> const & boundRects() const;

  /// react on touch events
  /// @{
  bool onTapStarted(m2::PointD const & pt);
  bool onTapMoved(m2::PointD const & pt);
  bool onTapEnded(m2::PointD const & pt);
  bool onTapCancelled(m2::PointD const & pt);
  /// @}
};
