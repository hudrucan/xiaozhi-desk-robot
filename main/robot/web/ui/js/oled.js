const oledMeta = {
  branding: ["Branding", "Robot identity", "Brand"],
  distance: ["Distance", "Floor range", "Dist"],
  power: ["Current / Power", "INA219 telemetry", "I / P"],
  motion: ["Motion sensor", "State and tilt", "Motion"],
  capacity: ["Battery measuring", "Capacity and time", "mAh"],
};

function oledModeOptions(type, selected) {
  const modes = type === "power"
    ? ["Current", "Power", "Current + Power"]
    : type === "motion"
      ? ["State", "Tilt", "State + Tilt"]
      : type === "capacity"
        ? ["mAh", "Elapsed", "mAh + elapsed"]
        : ["Default"];
  return modes.map((name, index) =>
    '<option value="' + index + '" ' + (index === selected ? "selected" : "") + ">" +
      name + "</option>",
  ).join("");
}

function oledPages(widgets) {
  const enabled = widgets.filter((widget) => widget.enabled);
  const pages = [];
  let cursor = 0;
  while (cursor < enabled.length) {
    const remaining = enabled.length - cursor;
    if (remaining >= 3 && enabled.slice(cursor, cursor + 3).every((widget) => widget.size === 0)) {
      pages.push(enabled.slice(cursor, cursor + 3).map((widget, index) => ({
        widget,
        x: index === 0 ? 0 : index === 1 ? 32.8125 : 66.40625,
        y: 0,
        width: index === 0 ? 32.8125 : 33.59375,
        height: 100,
      })));
      cursor += 3;
    } else if (remaining >= 2 && enabled[cursor].size < 2 && enabled[cursor + 1].size < 2) {
      pages.push([
        { widget: enabled[cursor], x: 0, y: 0, width: 50, height: 100 },
        { widget: enabled[cursor + 1], x: 50, y: 0, width: 50, height: 100 },
      ]);
      cursor += 2;
    } else {
      pages.push([{ widget: enabled[cursor], x: 0, y: 0, width: 100, height: 100 }]);
      cursor++;
    }
  }
  return pages;
}

function bindOledEditorActions() {
  $$('[data-oled-on]').forEach((element) => {
    element.onchange = () => action("oled_widget_on_" + element.dataset.oledOn, {
      value: element.checked ? 1 : 0,
    });
  });
  $$('[data-oled-size]').forEach((element) => {
    element.onchange = () => action("oled_widget_size_" + element.dataset.oledSize, {
      value: +element.value,
    });
  });
  $$('[data-oled-mode]').forEach((element) => {
    element.onchange = () => action("oled_widget_mode_" + element.dataset.oledMode, {
      value: +element.value,
    });
  });
  $$('[data-oled-up]').forEach((element) => {
    element.onclick = () => action("oled_widget_up_" + element.dataset.oledUp);
  });
  $$('[data-oled-down]').forEach((element) => {
    element.onclick = () => action("oled_widget_down_" + element.dataset.oledDown);
  });
  $$('[data-oled-custom]').forEach((element) => {
    element.onchange = () => action(
      element.dataset.oledCustom === "brand" ? "oled_brand" : "oled_prefix",
      { text: element.value },
    );
    element.onkeydown = (event) => {
      if (event.key === "Enter") element.blur();
    };
  });
}

function oledCustomEditor(widget, status) {
  if (widget.type === "branding") {
    return '<label class="oled-custom"><span>Custom text</span>' +
      '<input class="text-input" data-oled-custom="brand" maxlength="20" value="' +
      escapeHtml(status.oled_brand || "") + '"></label>';
  }
  if (widget.type === "distance") {
    return '<label class="oled-custom"><span>Custom prefix</span>' +
      '<input class="text-input" data-oled-custom="prefix" maxlength="10" value="' +
      escapeHtml(status.oled_distance_prefix || "") + '"></label>';
  }
  return "";
}

function renderOledEditor(status, widgets) {
  const signature = JSON.stringify([widgets, status.oled_brand, status.oled_distance_prefix]);
  const activeElement = document.activeElement;
  const editingCustomText = !!(activeElement?.matches?.('[data-oled-custom]'));
  if (signature === oledWidgetSignature || editingCustomText) return;

  oledWidgetSignature = signature;
  $("#oledWidgets").innerHTML = widgets.map((widget, index) => {
    const meta = oledMeta[widget.type] || [widget.type, ""];
    const modeDisabled = widget.type === "branding" || widget.type === "distance";
    return '<div class="oled-widget"><div class="oled-order">' +
      '<button data-oled-up="' + index + '" ' + (index === 0 ? "disabled" : "") +
      ' aria-label="Move up">↑</button>' +
      '<button data-oled-down="' + index + '" ' +
      (index === widgets.length - 1 ? "disabled" : "") +
      ' aria-label="Move down">↓</button></div>' +
      '<label class="toggle"><input type="checkbox" data-oled-on="' + index + '" ' +
      (widget.enabled ? "checked" : "") + '></label>' +
      '<div class="oled-widget-name"><b>' + escapeHtml(meta[0]) + "</b><small>" +
      escapeHtml(meta[1]) + '</small></div>' +
      '<select data-oled-size="' + index + '">' +
      '<option value="0" ' + (widget.size === 0 ? "selected" : "") + '>S · compact</option>' +
      '<option value="1" ' + (widget.size === 1 ? "selected" : "") + '>M · medium</option>' +
      '<option value="2" ' + (widget.size === 2 ? "selected" : "") + '>L · full</option></select>' +
      '<select data-oled-mode="' + index + '" ' + (modeDisabled ? "disabled" : "") + ">" +
      oledModeOptions(widget.type, widget.mode) + "</select>" +
      oledCustomEditor(widget, status) + "</div>";
  }).join("");
  bindOledEditorActions();
}

function renderOledPreview(status, widgets) {
  const pages = oledPages(widgets);
  const reported = Number(status.oled_page_count);
  const pageCount = Number.isFinite(reported) && reported > 0 ? reported : pages.length;
  $("#oledPageCount").textContent = pageCount + " page" + (pageCount === 1 ? "" : "s");

  const signature = JSON.stringify([widgets, pageCount]);
  if (signature === oledPreviewSignature) return;
  oledPreviewSignature = signature;
  $("#oledPreview").innerHTML = pages.length
    ? pages.map((page, pageIndex) =>
      '<div class="oled-page">' + page.map((cell) =>
        '<div class="oled-cell" style="left:' + cell.x + "%;top:" + cell.y +
        "%;width:" + cell.width + "%;height:" + cell.height + '%">' +
        escapeHtml((oledMeta[cell.widget.type] || [cell.widget.type, "", cell.widget.type])[2]) +
        "</div>",
      ).join("") + '<span class="oled-page-num">' + (pageIndex + 1) + "/" + pages.length +
      "</span></div>",
    ).join("")
    : '<div class="oled-empty">No widgets enabled</div>';
}

function renderOledConfig(status) {
  if (!Array.isArray(status.oled_widgets)) return;
  const widgets = status.oled_widgets.map((widget) => ({
    type: String(widget.type || "branding"),
    enabled: !!widget.enabled,
    size: Math.max(0, Math.min(2, Number(widget.size) || 0)),
    mode: Math.max(0, Math.min(2, Number(widget.mode) || 0)),
  }));
  renderOledEditor(status, widgets);
  renderOledPreview(status, widgets);
}
