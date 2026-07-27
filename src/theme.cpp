#include "theme.hpp"

namespace theme {

lv_style_t stylePlain;
lv_style_t styleCard;
lv_style_t styleRaised;
lv_style_t styleCaption;
lv_style_t styleValue;
lv_style_t styleButton;
lv_style_t styleButtonPrimary;
lv_style_t styleField;

void init() {
  // Layout containers that exist only to group children: no fill, no border,
  // no padding. The three panels each used to reinvent this.
  lv_style_init(&stylePlain);
  lv_style_set_bg_opa(&stylePlain, LV_OPA_TRANSP);
  lv_style_set_border_width(&stylePlain, 0);
  lv_style_set_pad_all(&stylePlain, 0);
  lv_style_set_radius(&stylePlain, 0);

  lv_style_init(&styleCard);
  lv_style_set_bg_color(&styleCard, c(kSurface));
  lv_style_set_bg_opa(&styleCard, LV_OPA_COVER);
  lv_style_set_radius(&styleCard, kRadLg);
  lv_style_set_border_width(&styleCard, 1);
  lv_style_set_border_color(&styleCard, c(kBorder));
  lv_style_set_border_opa(&styleCard, LV_OPA_60);
  lv_style_set_pad_all(&styleCard, kSp4);

  lv_style_init(&styleRaised);
  lv_style_set_bg_color(&styleRaised, c(kSurfaceHi));
  lv_style_set_bg_opa(&styleRaised, LV_OPA_COVER);
  lv_style_set_border_width(&styleRaised, 0);
  lv_style_set_radius(&styleRaised, kRadMd);

  lv_style_init(&styleCaption);
  lv_style_set_text_color(&styleCaption, c(kTextMuted));
  lv_style_set_text_font(&styleCaption, fontCaption());
  lv_style_set_text_letter_space(&styleCaption, 1);

  lv_style_init(&styleValue);
  lv_style_set_text_color(&styleValue, c(kText));
  lv_style_set_text_font(&styleValue, fontBody());

  lv_style_init(&styleButton);
  lv_style_set_bg_color(&styleButton, c(kSurfaceHi));
  lv_style_set_bg_opa(&styleButton, LV_OPA_COVER);
  lv_style_set_border_width(&styleButton, 0);
  lv_style_set_radius(&styleButton, kRadMd);

  // Only the fill differs from styleButton, but a second style beats a runtime
  // branch at every call site.
  lv_style_init(&styleButtonPrimary);
  lv_style_set_bg_color(&styleButtonPrimary, c(kAccent));
  lv_style_set_bg_opa(&styleButtonPrimary, LV_OPA_COVER);
  lv_style_set_border_width(&styleButtonPrimary, 0);
  lv_style_set_radius(&styleButtonPrimary, kRadMd);

  lv_style_init(&styleField);
  lv_style_set_bg_color(&styleField, c(kSurfaceHi));
  lv_style_set_border_color(&styleField, c(kTextMuted));
  lv_style_set_border_width(&styleField, 1);
  lv_style_set_text_color(&styleField, c(kText));
  lv_style_set_text_font(&styleField, fontBody());
}

}  // namespace theme
