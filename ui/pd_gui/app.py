#!/usr/bin/env python3
"""GTK4 / libadwaita front-end for payload-dumper.

Run through ``ui/payload-dumper-gui`` or ``python3 -m pd_gui`` from the ui
directory.  The window is a thin wrapper: it builds the CLI argument vector,
streams ``-m`` progress lines into progress bars, and shows stderr in a log.
"""

from __future__ import annotations

import json
import os
import shlex
import sys
import threading
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")

from gi.repository import Adw, Gio, GLib, Gtk, Pango  # noqa: E402

try:  # works both as a package and as a plain script
    from . import backend
except ImportError:  # pragma: no cover - direct script execution
    import backend  # type: ignore

APP_ID = "io.github.payloaddumper.Gui"
CONFIG_DIR = os.path.join(GLib.get_user_config_dir(), "payload-dumper-gui")
CONFIG_PATH = os.path.join(CONFIG_DIR, "config.json")


def _load_config() -> dict:
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _save_config(config: dict) -> None:
    try:
        os.makedirs(CONFIG_DIR, exist_ok=True)
        with open(CONFIG_PATH, "w", encoding="utf-8") as handle:
            json.dump(config, handle, ensure_ascii=False, indent=2)
    except OSError:
        pass


def _heading(text: str) -> Gtk.Label:
    label = Gtk.Label(label=text, xalign=0)
    label.add_css_class("heading")
    return label


def _dim(text: str = "") -> Gtk.Label:
    label = Gtk.Label(label=text, xalign=0)
    label.add_css_class("dim-label")
    return label


def _flat_icon_button(icon_name: str, tooltip: str) -> Gtk.Button:
    button = Gtk.Button(icon_name=icon_name)
    button.add_css_class("flat")
    button.set_valign(Gtk.Align.CENTER)
    button.set_tooltip_text(tooltip)
    return button


class PayloadDumperWindow(Adw.ApplicationWindow):
    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self.config = _load_config()
        self.binary_override = str(self.config.get("binary") or "")
        self._resolved_binary = backend.find_binary(self.binary_override or None)

        self.running = False
        self.listing = False
        self.cancellation: backend.Cancellation | None = None
        self.run_started = 0.0
        self.run_output_dir = ""
        self._tick_id = 0
        self._kill_timer_id = 0
        self._closed = False

        self.partitions: list[backend.Partition] = []
        self.partition_widgets: dict[str, tuple[Gtk.Widget, Gtk.CheckButton]] = {}
        self.partition_sizes: dict[str, int] = {}
        self.progress_rows: dict[str, tuple[Gtk.ProgressBar, Gtk.Label]] = {}
        self.progress_sizes: dict[str, int] = {}
        self.listed_input = ""
        self.log_line_count = 0

        self.set_title("Payload Dumper")
        self.set_size_request(760, 560)
        width = max(760, int(self.config.get("width") or 1020))
        height = max(560, int(self.config.get("height") or 880))
        self.set_default_size(width, height)
        self.connect("close-request", self._on_close_request)

        self._build_ui()
        self._refresh_binary_row()
        self._update_start_sensitivity()

    # ------------------------------------------------------------------ UI

    def _build_ui(self) -> None:
        toolbar = Adw.ToolbarView()

        header = Adw.HeaderBar()
        header.set_title_widget(
            Adw.WindowTitle(title="Payload Dumper", subtitle="Android OTA payload 解包")
        )
        self.settings_button = Gtk.MenuButton(
            icon_name="emblem-system-symbolic", tooltip_text="高级选项"
        )
        self.settings_button.set_popover(self._build_settings_popover())
        header.pack_end(self.settings_button)
        toolbar.add_top_bar(header)

        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        content.set_margin_top(14)
        content.set_margin_bottom(14)
        content.set_margin_start(18)
        content.set_margin_end(18)

        self.banner = Adw.Banner(title="")
        self.banner.set_revealed(False)
        content.append(self.banner)

        content.append(self._build_file_section())
        content.append(self._build_partition_section())
        content.append(self._build_progress_section())

        self.log_expander = Gtk.Expander(label="运行日志")
        self.log_buffer = Gtk.TextBuffer()
        self.log_view = Gtk.TextView(
            buffer=self.log_buffer,
            editable=False,
            cursor_visible=False,
            monospace=True,
            wrap_mode=Gtk.WrapMode.WORD_CHAR,
            top_margin=6,
            bottom_margin=6,
            left_margin=8,
            right_margin=8,
        )
        log_scroll = Gtk.ScrolledWindow(
            min_content_height=90, max_content_height=170, vexpand=True
        )
        log_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        log_scroll.add_css_class("card")
        log_scroll.set_child(self.log_view)
        self.log_expander.set_child(log_scroll)
        content.append(self.log_expander)

        content.append(self._build_actions())

        toolbar.set_content(content)
        self.set_content(toolbar)

    def _build_file_section(self) -> Gtk.Widget:
        section = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        section.append(_heading("输入文件"))

        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.input_entry = Gtk.Entry(
            hexpand=True,
            placeholder_text="payload.bin 或包含 payload.bin 的 OTA zip",
        )
        self.input_entry.connect("changed", self._on_input_changed)
        row.append(self.input_entry)

        self.input_browse = _flat_icon_button("folder-open-symbolic", "选择输入文件")
        self.input_browse.connect("clicked", self._choose_input)
        row.append(self.input_browse)

        self.list_button = Gtk.Button(label="读取分区")
        self.list_button.set_tooltip_text("读取 payload 中的分区列表，以便只解包选中的分区")
        self.list_button.connect("clicked", self._read_partitions)
        row.append(self.list_button)
        section.append(row)

        section.append(_heading("输出目录"))
        out_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.output_entry = Gtk.Entry(
            hexpand=True,
            placeholder_text="留空 = 当前目录/extracted_时间戳",
        )
        out_row.append(self.output_entry)
        self.output_browse = _flat_icon_button("folder-open-symbolic", "选择输出目录")
        self.output_browse.connect(
            "clicked",
            lambda *_: self._choose_folder(self.output_entry, "选择输出目录"),
        )
        out_row.append(self.output_browse)
        section.append(out_row)
        return section

    def _build_partition_section(self) -> Gtk.Widget:
        section = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)

        header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        header.append(_heading("分区"))
        self.partition_summary = _dim("尚未读取")
        self.partition_summary.set_hexpand(True)
        self.partition_summary.set_ellipsize(Pango.EllipsizeMode.END)
        header.append(self.partition_summary)

        self.select_all_button = Gtk.Button(label="全选")
        self.select_all_button.add_css_class("flat")
        self.select_all_button.set_sensitive(False)
        self.select_all_button.connect("clicked", lambda *_: self._set_all_checks(True))
        header.append(self.select_all_button)

        self.select_none_button = Gtk.Button(label="全不选")
        self.select_none_button.add_css_class("flat")
        self.select_none_button.set_sensitive(False)
        self.select_none_button.connect("clicked", lambda *_: self._set_all_checks(False))
        header.append(self.select_none_button)
        section.append(header)

        self.partition_filter = Gtk.SearchEntry(
            placeholder_text="过滤分区…", sensitive=False
        )
        self.partition_filter.connect("search-changed", self._on_filter_changed)
        section.append(self.partition_filter)

        self.partition_scroll = Gtk.ScrolledWindow(
            min_content_height=180, max_content_height=260
        )
        self.partition_scroll.set_policy(
            Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC
        )
        self.partition_scroll.add_css_class("card")
        self.partition_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        self.partition_box.set_margin_top(6)
        self.partition_box.set_margin_bottom(6)
        self.partition_box.set_margin_start(8)
        self.partition_box.set_margin_end(8)
        self.partition_scroll.set_child(self.partition_box)
        section.append(self.partition_scroll)

        self._show_partition_placeholder(
            "选择输入文件后点击“读取分区”，或直接开始解包全部分区"
        )
        return section

    def _build_progress_section(self) -> Gtk.Widget:
        section = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        section.set_vexpand(True)

        header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        header.append(_heading("进度"))
        self.overall_hint = _dim("")
        self.overall_hint.set_hexpand(True)
        self.overall_hint.set_ellipsize(Pango.EllipsizeMode.END)
        header.append(self.overall_hint)
        section.append(header)

        self.overall_bar = Gtk.ProgressBar()
        self.overall_bar.set_fraction(0.0)
        section.append(self.overall_bar)

        self.progress_scroll = Gtk.ScrolledWindow(
            vexpand=True, min_content_height=100
        )
        self.progress_scroll.set_policy(
            Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC
        )
        self.progress_scroll.add_css_class("card")
        self.progress_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        self.progress_box.set_margin_top(6)
        self.progress_box.set_margin_bottom(6)
        self.progress_box.set_margin_start(8)
        self.progress_box.set_margin_end(8)
        self.progress_scroll.set_child(self.progress_box)
        section.append(self.progress_scroll)

        self._show_progress_placeholder("开始解包后显示各分区进度")
        return section

    def _build_actions(self) -> Gtk.Widget:
        actions = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.status_label = Gtk.Label(label="就绪", xalign=0)
        self.status_label.add_css_class("dim-label")
        self.status_label.set_hexpand(True)
        self.status_label.set_ellipsize(Pango.EllipsizeMode.END)
        actions.append(self.status_label)

        self.open_output_button = Gtk.Button(label="打开输出目录")
        self.open_output_button.set_visible(False)
        self.open_output_button.connect("clicked", self._open_output_dir)
        actions.append(self.open_output_button)

        self.stop_button = Gtk.Button(label="停止")
        self.stop_button.add_css_class("destructive-action")
        self.stop_button.set_sensitive(False)
        self.stop_button.connect("clicked", self._stop_extraction)
        actions.append(self.stop_button)

        self.start_button = Gtk.Button(label="开始解包")
        self.start_button.add_css_class("suggested-action")
        self.start_button.connect("clicked", self._start_extraction)
        actions.append(self.start_button)
        return actions

    def _build_settings_popover(self) -> Gtk.Popover:
        popover = Gtk.Popover()
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        box.set_margin_top(12)
        box.set_margin_bottom(12)
        box.set_margin_start(12)
        box.set_margin_end(12)
        box.set_size_request(460, -1)

        group = Adw.PreferencesGroup(title="高级选项")

        self.workers_row = Adw.SpinRow.new_with_range(0, 256, 1)
        self.workers_row.set_title("解包 worker 数")
        self.workers_row.set_subtitle("0 = 自动（逻辑 CPU 数）；校验 worker 为其一半")
        self.workers_row.set_value(int(self.config.get("workers") or 0))
        group.add(self.workers_row)

        self.old_row = Adw.EntryRow(title="旧版镜像目录（-old）")
        self.old_row.set_text(str(self.config.get("old_dir") or ""))
        old_browse = _flat_icon_button("folder-open-symbolic", "选择旧版镜像目录")
        old_browse.connect(
            "clicked", lambda *_: self._choose_folder(self.old_row, "选择旧版镜像目录")
        )
        self.old_row.add_suffix(old_browse)
        group.add(self.old_row)

        self.no_verify_row = Adw.SwitchRow(
            title="跳过校验（-no-verify）",
            subtitle="仅跳过源数据与最终镜像校验，operation blob 仍会校验",
        )
        self.no_verify_row.set_active(bool(self.config.get("no_verify")))
        group.add(self.no_verify_row)

        self.no_fec_row = Adw.SwitchRow(
            title="跳过 FEC（-no-fec）",
            subtitle="生成的镜像不可刷写",
        )
        self.no_fec_row.set_active(bool(self.config.get("no_fec")))
        group.add(self.no_fec_row)

        self.binary_row = Adw.ActionRow(title="payload-dumper 可执行文件")
        binary_button = Gtk.Button(label="选择…")
        binary_button.add_css_class("flat")
        binary_button.set_valign(Gtk.Align.CENTER)
        binary_button.connect("clicked", self._choose_binary)
        self.binary_row.add_suffix(binary_button)
        group.add(self.binary_row)

        box.append(group)
        popover.set_child(box)
        return popover

    # ------------------------------------------------------------- helpers

    def _input_path(self) -> str:
        text = self.input_entry.get_text().strip()
        return os.path.abspath(os.path.expanduser(text)) if text else ""

    def _refresh_binary_row(self) -> None:
        self._resolved_binary = backend.find_binary(self.binary_override or None)
        if self._resolved_binary:
            self.binary_row.set_subtitle(self._resolved_binary)
        else:
            self.binary_row.set_subtitle(
                "未找到；请先构建 build/release/payload-dumper 或手动选择"
            )
        self._update_start_sensitivity()

    def _show_banner(self, text: str) -> None:
        self.banner.set_title(text)
        self.banner.set_revealed(True)

    def _hide_banner(self) -> None:
        self.banner.set_revealed(False)

    def _show_partition_placeholder(self, text: str) -> None:
        while (child := self.partition_box.get_first_child()) is not None:
            self.partition_box.remove(child)
        placeholder = Gtk.Label(label=text, wrap=True, margin_top=16, margin_bottom=16)
        placeholder.add_css_class("dim-label")
        self.partition_box.append(placeholder)

    def _show_progress_placeholder(self, text: str) -> None:
        while (child := self.progress_box.get_first_child()) is not None:
            self.progress_box.remove(child)
        placeholder = Gtk.Label(label=text, wrap=True, margin_top=16, margin_bottom=16)
        placeholder.add_css_class("dim-label")
        self.progress_box.append(placeholder)

    def _set_controls_enabled(self, enabled: bool) -> None:
        for widget in (
            self.input_entry,
            self.input_browse,
            self.output_entry,
            self.output_browse,
            self.list_button,
            self.settings_button,
        ):
            widget.set_sensitive(enabled)
        has_partitions = bool(self.partitions)
        self.select_all_button.set_sensitive(enabled and has_partitions)
        self.select_none_button.set_sensitive(enabled and has_partitions)
        self.partition_filter.set_sensitive(enabled and has_partitions)
        for _part, (row, check) in self.partition_widgets.items():
            row.set_sensitive(enabled)
            check.set_sensitive(enabled)

    def _update_start_sensitivity(self) -> None:
        if getattr(self, "start_button", None) is None:
            return
        if self.running or self.listing:
            self.start_button.set_sensitive(False)
            return
        binary_ok = bool(
            self._resolved_binary
            and os.path.isfile(self._resolved_binary)
            and os.access(self._resolved_binary, os.X_OK)
        )
        input_text = self._input_path()
        input_ok = bool(input_text and os.path.isfile(input_text))
        selection_ok = True
        if self.partitions:
            selection_ok = any(
                check.get_active() for _row, check in self.partition_widgets.values()
            )
        self.start_button.set_sensitive(binary_ok and input_ok and selection_ok)

    def _selected_partitions(self) -> list[backend.Partition]:
        return [
            part
            for part in self.partitions
            if (widgets := self.partition_widgets.get(part.name))
            and widgets[1].get_active()
        ]

    def _update_selection_summary(self) -> None:
        if not self.partitions:
            self.partition_summary.set_text("尚未读取")
            return
        selected = self._selected_partitions()
        total = sum(part.size for part in selected)
        self.partition_summary.set_text(
            f"已选 {len(selected)}/{len(self.partitions)} · {backend.format_size(total)}"
        )
        self._update_start_sensitivity()

    def _set_all_checks(self, active: bool) -> None:
        if self.running or self.listing:
            return
        for _row, check in self.partition_widgets.values():
            check.set_active(active)
        self._update_selection_summary()

    def _on_filter_changed(self, entry: Gtk.SearchEntry) -> None:
        query = entry.get_text().strip().lower()
        for part, (row, _check) in self.partition_widgets.items():
            row.set_visible(not query or query in part.name.lower())

    # ----------------------------------------------------------- file pick

    def _pick_file(self, title: str, initial: str, callback, patterns=None) -> None:
        dialog = Gtk.FileDialog()
        dialog.set_title(title)
        dialog.set_modal(True)
        if patterns:
            filters = Gio.ListStore.new(Gtk.FileFilter)
            file_filter = Gtk.FileFilter()
            file_filter.set_name("Payload 与 OTA 文件")
            for pattern in patterns:
                file_filter.add_pattern(pattern)
            filters.append(file_filter)
            all_filter = Gtk.FileFilter()
            all_filter.set_name("所有文件")
            all_filter.add_pattern("*")
            filters.append(all_filter)
            dialog.set_filters(filters)
            dialog.set_default_filter(file_filter)
        if initial:
            folder = initial if os.path.isdir(initial) else os.path.dirname(initial)
            if folder and os.path.isdir(folder):
                dialog.set_initial_folder(Gio.File.new_for_path(folder))
        dialog.open(self, None, callback)

    def _pick_folder(self, title: str, initial: str, callback) -> None:
        dialog = Gtk.FileDialog()
        dialog.set_title(title)
        dialog.set_modal(True)
        if initial:
            path = initial
            if not os.path.isdir(path) and os.path.isdir(os.path.dirname(path)):
                path = os.path.dirname(path)
            if os.path.isdir(path):
                dialog.set_initial_folder(Gio.File.new_for_path(path))
        dialog.select_folder(self, None, callback)

    def _choose_input(self, *_args) -> None:
        self._pick_file(
            "选择 payload.bin 或 OTA zip",
            self._input_path() or os.path.expanduser("~"),
            self._on_input_chosen,
            patterns=["*.bin", "*.zip"],
        )

    def _on_input_chosen(self, dialog, result) -> None:
        try:
            gfile = dialog.open_finish(result)
        except GLib.Error:
            return
        if gfile is not None and gfile.get_path():
            self.input_entry.set_text(gfile.get_path())

    def _choose_folder(self, entry, title: str) -> None:
        initial = (
            entry.get_text().strip() or self._input_path() or os.path.expanduser("~")
        )

        def on_chosen(dialog, result) -> None:
            try:
                gfile = dialog.select_folder_finish(result)
            except GLib.Error:
                return
            if gfile is not None and gfile.get_path():
                entry.set_text(gfile.get_path())

        self._pick_folder(title, initial, on_chosen)

    def _choose_binary(self, *_args) -> None:
        def on_chosen(dialog, result) -> None:
            try:
                gfile = dialog.open_finish(result)
            except GLib.Error:
                return
            if gfile is not None and gfile.get_path():
                self.binary_override = gfile.get_path()
                self._refresh_binary_row()
                self._hide_banner()

        self._pick_file(
            "选择 payload-dumper 可执行文件",
            self.binary_override or backend.REPO_ROOT,
            on_chosen,
        )

    # --------------------------------------------------------------- input

    def _on_input_changed(self, entry: Gtk.Entry) -> None:
        if self.running or self.listing:
            return
        current = self._input_path()
        if self.listed_input and current != self.listed_input:
            self._clear_partitions("输入已更改，请重新读取分区")
        self._update_start_sensitivity()

    def _clear_partitions(self, placeholder: str) -> None:
        self.partitions = []
        self.partition_widgets.clear()
        self.partition_sizes.clear()
        self.listed_input = ""
        self.partition_filter.set_sensitive(False)
        self.partition_filter.set_text("")
        self.select_all_button.set_sensitive(False)
        self.select_none_button.set_sensitive(False)
        self.partition_summary.set_text("尚未读取")
        self._show_partition_placeholder(placeholder)
        self._update_start_sensitivity()

    def _read_partitions(self, *_args) -> None:
        if self.running or self.listing:
            return
        input_path = self._input_path()
        if not input_path or not os.path.isfile(input_path):
            self._show_banner("请选择一个存在的 payload.bin 或 OTA zip")
            return
        binary = self._resolved_binary
        if not binary:
            self._show_banner("未找到 payload-dumper，请在高级选项中指定可执行文件")
            return

        self._hide_banner()
        self.listing = True
        self._set_controls_enabled(False)
        self.list_button.set_sensitive(False)
        self.list_button.set_label("读取中…")
        self.status_label.set_text("正在读取分区…")
        self._update_start_sensitivity()

        def work() -> None:
            try:
                parts = backend.list_partitions(binary, input_path)
            except Exception as exc:  # noqa: BLE001 - surfaced to the user
                GLib.idle_add(self._partitions_failed, str(exc))
            else:
                GLib.idle_add(self._partitions_loaded, input_path, parts)

        threading.Thread(target=work, name="pd-list", daemon=True).start()

    def _finish_listing_ui(self) -> None:
        self.listing = False
        self._set_controls_enabled(True)
        self.list_button.set_label("重新读取")
        self.list_button.set_sensitive(True)
        self._update_start_sensitivity()

    def _partitions_failed(self, message: str) -> bool:
        if self._closed:
            return False
        self._finish_listing_ui()
        self.status_label.set_text("读取分区失败")
        self._show_banner(f"读取分区失败：{message}")
        self._append_log(f"Error: {message}")
        self.log_expander.set_label("运行日志（错误）")
        return False

    def _partitions_loaded(self, path: str, parts: list) -> bool:
        if self._closed:
            return False
        self._finish_listing_ui()
        self.partitions = list(parts)
        self.partition_widgets.clear()
        self.partition_sizes = {part.name: part.size for part in self.partitions}
        self.listed_input = path

        while (child := self.partition_box.get_first_child()) is not None:
            self.partition_box.remove(child)

        if not self.partitions:
            self._show_partition_placeholder("未发现分区")
            self.partition_summary.set_text("0 个分区")
            self.status_label.set_text("未发现分区")
            return False

        for part in self.partitions:
            row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
            check = Gtk.CheckButton(label=part.name, active=True)
            check.connect("toggled", self._on_partition_toggled)
            row.append(check)
            if part.delta:
                tag = Gtk.Label(label="增量")
                tag.add_css_class("accent")
                tag.set_tooltip_text("该分区需要旧版镜像（-old）")
                row.append(tag)
            spacer = Gtk.Box()
            spacer.set_hexpand(True)
            row.append(spacer)
            size_label = Gtk.Label(label=backend.format_size(part.size), xalign=1)
            size_label.add_css_class("dim-label")
            row.append(size_label)
            self.partition_box.append(row)
            self.partition_widgets[part.name] = (row, check)

        self.partition_filter.set_sensitive(True)
        self.select_all_button.set_sensitive(True)
        self.select_none_button.set_sensitive(True)
        self._update_selection_summary()

        total_size = sum(part.size for part in self.partitions)
        delta_count = sum(1 for part in self.partitions if part.delta)
        summary = (
            f"已读取 {len(self.partitions)} 个分区 · 共 {backend.format_size(total_size)}"
        )
        if delta_count:
            summary += f" · {delta_count} 个增量分区"
        self.status_label.set_text(summary)
        if delta_count:
            self._show_banner(
                "检测到增量分区：增量 OTA 需在高级选项中选择旧版镜像目录（-old）"
            )
        self._update_start_sensitivity()
        return False

    def _on_partition_toggled(self, *_args) -> None:
        self._update_selection_summary()

    # ------------------------------------------------------------ progress

    def _reset_progress(self) -> None:
        while (child := self.progress_box.get_first_child()) is not None:
            self.progress_box.remove(child)
        self.progress_rows.clear()
        self.progress_sizes.clear()
        self.overall_bar.set_fraction(0.0)
        self.overall_hint.set_text("")

    def _ensure_progress_row(self, name: str) -> None:
        if name in self.progress_rows:
            return
        if not self.progress_rows:
            while (child := self.progress_box.get_first_child()) is not None:
                self.progress_box.remove(child)
        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        name_label = Gtk.Label(label=name, xalign=0)
        name_label.add_css_class("monospace")
        name_label.set_size_request(150, -1)
        name_label.set_ellipsize(Pango.EllipsizeMode.END)
        row.append(name_label)
        bar = Gtk.ProgressBar(hexpand=True)
        bar.set_fraction(0.0)
        row.append(bar)
        percent_label = Gtk.Label(label="0%", xalign=1)
        percent_label.set_size_request(42, -1)
        percent_label.add_css_class("monospace")
        row.append(percent_label)
        self.progress_box.append(row)
        self.progress_rows[name] = (bar, percent_label)
        self.progress_sizes[name] = self.partition_sizes.get(name, 0)

    def _update_overall(self) -> None:
        total_weight = 0.0
        done_weight = 0.0
        for name, (bar, _label) in self.progress_rows.items():
            weight = float(self.progress_sizes.get(name) or 1)
            total_weight += weight
            done_weight += weight * bar.get_fraction()
        fraction = done_weight / total_weight if total_weight else 0.0
        self.overall_bar.set_fraction(fraction)
        if self.progress_rows:
            self.overall_hint.set_text(
                f"{len(self.progress_rows)} 个分区 · {fraction * 100:.0f}%"
            )
        else:
            self.overall_hint.set_text("")

    def _handle_event(self, event) -> bool:
        if self._closed:
            return False
        if isinstance(event, backend.Progress):
            if not self.running:
                return False
            self._ensure_progress_row(event.partition)
            bar, label = self.progress_rows[event.partition]
            bar.set_fraction(event.percent / 100.0)
            label.set_text(f"{event.percent}%")
            self._update_overall()
        elif isinstance(event, backend.LogLine):
            self._append_log(event.text)
        elif isinstance(event, backend.Finished):
            self._on_finished(event)
        return False

    def _append_log(self, text: str) -> None:
        buffer = self.log_buffer
        if self.log_line_count >= 5000:
            buffer.set_text("")
            self.log_line_count = 0
        end = buffer.get_end_iter()
        if self.log_line_count:
            buffer.insert(end, "\n")
        buffer.insert(buffer.get_end_iter(), text)
        self.log_line_count += 1 + text.count("\n")
        self.log_view.scroll_to_iter(buffer.get_end_iter(), 0.0, False, 0.0, 1.0)

    # ------------------------------------------------------------ commands

    def _start_extraction(self, *_args) -> None:
        if self.running or self.listing:
            return
        input_path = self._input_path()
        if not input_path or not os.path.isfile(input_path):
            self._show_banner("请选择一个存在的 payload.bin 或 OTA zip")
            return
        binary = self._resolved_binary
        if not binary:
            self._show_banner("未找到 payload-dumper，请在高级选项中指定可执行文件")
            return

        selected = self._selected_partitions()
        if self.partitions and not selected:
            self._show_banner("请至少选择一个分区")
            return

        output_dir = self.output_entry.get_text().strip()
        if not output_dir:
            output_dir = backend.default_output_dir()
            self.output_entry.set_text(output_dir)
        else:
            output_dir = os.path.abspath(os.path.expanduser(output_dir))
            self.output_entry.set_text(output_dir)

        old_dir = self.old_row.get_text().strip()
        old_dir = os.path.abspath(os.path.expanduser(old_dir)) if old_dir else None
        if old_dir and not os.path.isdir(old_dir):
            self._show_banner(f"旧版镜像目录不存在：{old_dir}")
            return

        # Pass -p only when a subset was chosen; the default is all partitions.
        partition_names = None
        if self.partitions and len(selected) < len(self.partitions):
            partition_names = [part.name for part in selected]
        elif self.partitions:
            partition_names = None

        command = backend.build_extract_command(
            binary,
            input_path,
            output_dir=output_dir,
            workers=int(self.workers_row.get_value()),
            partitions=partition_names,
            old_dir=old_dir,
            no_verify=self.no_verify_row.get_active(),
            no_fec=self.no_fec_row.get_active(),
        )

        self._hide_banner()
        self._reset_progress()
        self.log_buffer.set_text("")
        self.log_line_count = 0
        self._append_log(f"$ {shlex.join(command)}")
        self.log_expander.set_label("运行日志")
        self.log_expander.set_expanded(False)

        for part in selected:
            self._ensure_progress_row(part.name)
        if not self.progress_rows:
            self._show_progress_placeholder("等待分区进度…")
        self._update_overall()

        self._cancel_kill_timer()
        self.running = True
        self.cancellation = backend.Cancellation()
        self.run_started = time.monotonic()
        self.run_output_dir = output_dir
        self.open_output_button.set_visible(False)
        self._set_controls_enabled(False)
        self.start_button.set_sensitive(False)
        self.stop_button.set_sensitive(True)
        self.status_label.set_text("解包中…")

        cancellation = self.cancellation

        def emit(event) -> None:
            GLib.idle_add(self._handle_event, event)

        def work() -> None:
            backend.run_extraction(command, os.getcwd(), emit, cancellation)

        threading.Thread(target=work, name="pd-extract", daemon=True).start()
        self._tick_id = GLib.timeout_add(200, self._tick)

    def _tick(self) -> bool:
        if self._closed or not self.running:
            self._tick_id = 0
            return GLib.SOURCE_REMOVE
        elapsed = time.monotonic() - self.run_started
        self.status_label.set_text(f"解包中… 已用时 {elapsed:.1f} s")
        return GLib.SOURCE_CONTINUE

    def _stop_extraction(self, *_args) -> None:
        if not self.running or self.cancellation is None:
            return
        cancellation = self.cancellation
        cancellation.request()
        self.stop_button.set_sensitive(False)
        self.status_label.set_text("正在停止…")
        self._cancel_kill_timer()
        self._kill_timer_id = GLib.timeout_add_seconds(
            5, self._force_kill, cancellation
        )

    def _cancel_kill_timer(self) -> None:
        if self._kill_timer_id:
            GLib.source_remove(self._kill_timer_id)
            self._kill_timer_id = 0

    def _force_kill(self, cancellation: backend.Cancellation) -> bool:
        self._kill_timer_id = 0
        if self.running and self.cancellation is cancellation:
            cancellation.force_kill()
        return GLib.SOURCE_REMOVE

    def _on_finished(self, event: backend.Finished) -> None:
        if not self.running:
            return
        self.running = False
        self.cancellation = None
        if self._tick_id:
            GLib.source_remove(self._tick_id)
            self._tick_id = 0
        self._cancel_kill_timer()

        self._finish_extraction_ui()

        if event.cancelled:
            self.status_label.set_text(f"已停止 · 用时 {event.elapsed:.1f} s")
            self.open_output_button.set_tooltip_text(self.run_output_dir)
            self.open_output_button.set_visible(
                bool(self.run_output_dir and os.path.isdir(self.run_output_dir))
            )
        elif event.error:
            self.status_label.set_text(f"解包失败 · 用时 {event.elapsed:.1f} s")
            self._show_banner(f"解包失败：{event.error}")
            self._append_log(f"Error: {event.error}")
            self.log_expander.set_label("运行日志（错误）")
        else:
            for bar, label in self.progress_rows.values():
                bar.set_fraction(1.0)
                label.set_text("100%")
            self.overall_bar.set_fraction(1.0)
            if self.progress_rows:
                self.overall_hint.set_text(
                    f"{len(self.progress_rows)} 个分区 · 100%"
                )
            self.status_label.set_text(f"完成 · 用时 {event.elapsed:.1f} s")
            self._append_log(f"完成：{self.run_output_dir}")
            self.open_output_button.set_tooltip_text(self.run_output_dir)
            self.open_output_button.set_visible(
                bool(self.run_output_dir and os.path.isdir(self.run_output_dir))
            )

    def _finish_extraction_ui(self) -> None:
        self._set_controls_enabled(True)
        self.stop_button.set_sensitive(False)
        self._update_start_sensitivity()

    def _open_output_dir(self, *_args) -> None:
        if not self.run_output_dir or not os.path.isdir(self.run_output_dir):
            self._show_banner("输出目录尚不存在")
            return
        try:
            Gio.AppInfo.launch_default_for_uri(
                GLib.filename_to_uri(self.run_output_dir, None), None
            )
        except GLib.Error as exc:
            self._show_banner(f"无法打开输出目录：{exc.message}")

    # --------------------------------------------------------------- close

    def _save_config(self) -> None:
        _save_config(
            {
                "binary": self.binary_override,
                "workers": int(self.workers_row.get_value()),
                "old_dir": self.old_row.get_text().strip(),
                "no_verify": self.no_verify_row.get_active(),
                "no_fec": self.no_fec_row.get_active(),
                "width": max(760, self.get_width() or 1020),
                "height": max(560, self.get_height() or 880),
            }
        )

    def _on_close_request(self, *_args) -> bool:
        if self.running and self.cancellation is not None:
            dialog = Adw.AlertDialog(
                heading="解包仍在进行",
                body="停止当前解包并退出？已写入的镜像可能不完整。",
            )
            dialog.add_response("cancel", "取消")
            dialog.add_response("quit", "停止并退出")
            dialog.set_response_appearance(
                "quit", Adw.ResponseAppearance.DESTRUCTIVE
            )
            if hasattr(dialog, "set_close_response"):
                dialog.set_close_response("cancel")
            dialog.connect("response", self._on_close_response)
            dialog.present(self)
            return True
        self._closed = True
        self._save_config()
        return False

    def _on_close_response(self, _dialog, response: str) -> None:
        if response == "quit":
            if self.cancellation is not None:
                self.cancellation.request()
            self._cancel_kill_timer()
            if self._tick_id:
                GLib.source_remove(self._tick_id)
                self._tick_id = 0
            self.running = False
            self._save_config()
            self._closed = True
            self.destroy()


class PayloadDumperApplication(Adw.Application):
    def __init__(self) -> None:
        super().__init__(
            application_id=APP_ID, flags=Gio.ApplicationFlags.DEFAULT_FLAGS
        )
        self.connect("activate", self._on_activate)

    def _on_activate(self, _app) -> None:
        window = self.get_active_window()
        if window is None:
            window = PayloadDumperWindow(application=self)
        window.present()


def main(argv: list[str] | None = None) -> int:
    GLib.set_application_name("Payload Dumper")
    app = PayloadDumperApplication()
    return app.run(argv if argv is not None else sys.argv)


if __name__ == "__main__":
    sys.exit(main())
