import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  Alert,
  AppBar,
  Avatar,
  Box,
  Button,
  CircularProgress,
  Dialog,
  DialogActions,
  DialogContent,
  DialogContentText,
  DialogTitle,
  Divider,
  FormControl,
  FormControlLabel,
  IconButton,
  InputAdornment,
  Menu,
  MenuItem,
  Select,
  Slider,
  Snackbar,
  Switch,
  TextField,
  ThemeProvider,
  Toolbar,
  Typography,
  useMediaQuery
} from "@mui/material";
import {
  AddRounded,
  ArrowDownwardRounded,
  ArrowForwardIosRounded,
  ArrowUpwardRounded,
  BusinessRounded,
  CalendarMonthRounded,
  CheckRounded,
  CloseRounded,
  CreateNewFolderRounded,
  DeleteOutlineRounded,
  DownloadRounded,
  EditRounded,
  ErrorOutlineRounded,
  FolderRounded,
  GamepadRounded,
  InsertDriveFileRounded,
  KeyboardRounded,
  MoreHorizRounded,
  RestartAltRounded,
  SearchRounded,
  SettingsRounded,
  StorageRounded,
  SortByAlphaRounded,
  StopCircleRounded,
  SwapHorizRounded,
  TuneRounded,
  UploadRounded
} from "@mui/icons-material";
import { EmulatorScreen } from "./EmulatorScreen";
import { readJarMetadata } from "./jarMetadata";
import { PhoneMEWebRuntime } from "./phoneMEClient";
import {
  applyPwaUpdate,
  checkForPwaUpdate,
  getPendingPwaUpdateVersion,
  PWA_UPDATE_READY_EVENT,
  type PwaUpdateReadyDetail
} from "./pwa";
import { createPhoneMETheme, appbarThemeColor } from "./theme";
import type {
  GameEntry,
  ManagedStorageEntry,
  ManagedStorageKind,
  RuntimePhase,
  RuntimeSnapshot,
  ThemePreference,
  ViewId
} from "./types";
import {
  DEFAULT_GAME_PROFILE,
  normalizeGameProfile,
  type ButtonShape,
  type KeyLayout,
  type ScaleType,
  type ScreenGravity,
  type VirtualKeyboardType,
  type WebGameProfile
} from "./webProfile";

const GAMES_KEY = "phoneme-web.games.v1";
const SETTINGS_KEY = "phoneme-web.settings.v2";
const LEGACY_SETTINGS_KEY = "phoneme-web.settings.v1";
const PROFILES_KEY = "phoneme-web.game-profiles.v1";
const LIBRARY_PREFS_KEY = "phoneme-web.library.v1";

const SCREEN_PROFILES = [
  { label: "128 × 128", width: 128, height: 128 },
  { label: "128 × 160", width: 128, height: 160 },
  { label: "176 × 220", width: 176, height: 220 },
  { label: "240 × 320", width: 240, height: 320 },
  { label: "320 × 240", width: 320, height: 240 },
  { label: "360 × 640", width: 360, height: 640 }
];

const HEAP_PRESETS = [16, 32, 64, 96, 128, 160, 192];

type AppSettings = {
  theme: ThemePreference;
  language: "system" | "vi" | "en";
  translationProvider: "bing" | "google" | "automatic";
  enableActionBar: boolean;
  enableStatusBar: boolean;
  keepScreenOn: boolean;
  websocketProxyUrl: string;
};

type LibrarySort = "name" | "date" | "vendor";
type LibraryPreferences = {
  sort: LibrarySort;
  descending: boolean;
};

const DEFAULT_WEBSOCKET_PROXY_URL = "wss://phoneme-websockify.fly.dev/?access=7f3bcf097f3878b2afc9d057865f384348ac1781d34f8b5f";

function defaultWebsocketProxyUrl() {
  return DEFAULT_WEBSOCKET_PROXY_URL;
}

function normalizeWebsocketProxyUrl(value: string | undefined) {
  const fallback = defaultWebsocketProxyUrl();
  if (!value) return fallback;
  try {
    const url = new URL(value);
    if (url.protocol !== "ws:" && url.protocol !== "wss:") return fallback;
    if (
      url.protocol === "ws:" &&
      url.port === "38473" &&
      (url.hostname === "127.0.0.1" || url.hostname === "localhost")
    ) {
      url.protocol = "wss:";
    }
    // The target token is generated per socket by phoneME.ts. Old local test
    // values sometimes persisted ?token=127.0.0.1:18081; never reuse it.
    url.searchParams.delete("token");
    return url.href.replace(/\/$/, "");
  } catch {
    return fallback;
  }
}

const DEFAULT_SETTINGS: AppSettings = {
  theme: "system",
  language: "system",
  translationProvider: "bing",
  enableActionBar: true,
  enableStatusBar: false,
  keepScreenOn: false,
  websocketProxyUrl: defaultWebsocketProxyUrl()
};

const DEFAULT_LIBRARY_PREFERENCES: LibraryPreferences = {
  sort: "name",
  descending: false
};

const EMPTY_SNAPSHOT: RuntimeSnapshot = {
  phase: "loading",
  message: "Đang nạp phoneME WebAssembly",
  fps: 0,
  usedMemory: 0,
  frameWidth: 0,
  frameHeight: 0
};

function readJson<T>(key: string, fallback: T): T {
  try {
    const value = localStorage.getItem(key);
    return value ? JSON.parse(value) as T : fallback;
  } catch {
    return fallback;
  }
}

function formatStorageSize(bytes: number) {
  if (!Number.isFinite(bytes) || bytes <= 0) return "0 B";
  const units = ["B", "KiB", "MiB", "GiB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value >= 10 || unit === 0 ? value.toFixed(0) : value.toFixed(1)} ${units[unit]}`;
}

function downloadBlob(blob: Blob, fileName: string) {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = fileName;
  anchor.style.display = "none";
  document.body.appendChild(anchor);
  anchor.click();
  anchor.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 1_000);
}

function downloadBytes(bytes: Uint8Array<ArrayBuffer>, fileName: string, contentType = "application/octet-stream") {
  downloadBlob(new Blob([bytes], { type: contentType }), fileName);
}

function StorageManagerView({ runtime, games, initialKind = "files" }: {
  runtime: PhoneMEWebRuntime;
  games: GameEntry[];
  initialKind?: ManagedStorageKind;
}) {
  const fileInputRef = useRef<HTMLInputElement>(null);
  const folderInputRef = useRef<HTMLInputElement>(null);
  const [kind, setKind] = useState<ManagedStorageKind>(initialKind);
  const [path, setPath] = useState("");
  const [entries, setEntries] = useState<ManagedStorageEntry[]>([]);
  const [loading, setLoading] = useState(false);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState<{ severity: "success" | "error" | "info"; message: string } | null>(null);
  const [newFolderOpen, setNewFolderOpen] = useState(false);
  const [newFolderName, setNewFolderName] = useState("");
  const [deleteEntry, setDeleteEntry] = useState<ManagedStorageEntry | null>(null);

  useEffect(() => {
    folderInputRef.current?.setAttribute("webkitdirectory", "");
  }, []);

  const loadEntries = useCallback(async () => {
    setLoading(true);
    try {
      setEntries(await runtime.listManagedStorage(kind, path));
    } catch (error) {
      setNotice({ severity: "error", message: error instanceof Error ? error.message : String(error) });
    } finally {
      setLoading(false);
    }
  }, [kind, path, runtime]);

  useEffect(() => {
    void loadEntries();
  }, [loadEntries]);

  const changeKind = (next: ManagedStorageKind) => {
    if (next === kind) return;
    setKind(next);
    setPath("");
    setNotice(null);
  };

  const upload = async (files: FileList | null, preserveFolder: boolean) => {
    const selected = Array.from(files ?? []);
    if (!selected.length) return;
    setBusy(true);
    try {
      const uploads = selected.map((file) => ({
        relativePath: preserveFolder && file.webkitRelativePath ? file.webkitRelativePath : file.name,
        file
      }));
      await runtime.importManagedStorageFiles(kind, path, uploads);
      setNotice({ severity: "success", message: `Đã tải lên ${selected.length} tệp` });
      await loadEntries();
    } catch (error) {
      setNotice({ severity: "error", message: error instanceof Error ? error.message : String(error) });
    } finally {
      setBusy(false);
    }
  };

  const downloadPath = async (relativePath: string) => {
    setBusy(true);
    try {
      const exported = await runtime.exportManagedStorage(kind, relativePath);
      if (!exported.isDirectory) {
        const file = exported.files[0];
        if (!file) throw new Error("Tệp không có dữ liệu");
        downloadBytes(file.data, exported.name);
      } else {
        if (!exported.archive) throw new Error("Không tạo được file ZIP");
        downloadBlob(exported.archive, `${exported.name}.zip`);
      }
      setNotice({ severity: "success", message: exported.isDirectory ? "Đã tạo file ZIP" : "Đã tải tệp" });
    } catch (error) {
      setNotice({ severity: "error", message: error instanceof Error ? error.message : String(error) });
    } finally {
      setBusy(false);
    }
  };

  const createFolder = async () => {
    const name = newFolderName.trim();
    if (!name || name === "." || name === ".." || /[\\/\0]/.test(name)) {
      setNotice({ severity: "error", message: "Tên thư mục không hợp lệ" });
      return;
    }
    setBusy(true);
    try {
      await runtime.createManagedStorageDirectory(kind, path ? `${path}/${name}` : name);
      setNewFolderOpen(false);
      setNewFolderName("");
      setNotice({ severity: "success", message: `Đã tạo thư mục ${name}` });
      await loadEntries();
    } catch (error) {
      setNotice({ severity: "error", message: error instanceof Error ? error.message : String(error) });
    } finally {
      setBusy(false);
    }
  };

  const confirmDelete = async () => {
    if (!deleteEntry) return;
    const target = deleteEntry;
    setBusy(true);
    try {
      await runtime.deleteManagedStorageEntry(kind, target.path);
      setDeleteEntry(null);
      setNotice({ severity: "success", message: `Đã xóa ${target.name}` });
      await loadEntries();
    } catch (error) {
      setNotice({ severity: "error", message: error instanceof Error ? error.message : String(error) });
    } finally {
      setBusy(false);
    }
  };

  const runningGame = runtime.activeGame;
  const pathParts = path ? path.split("/") : [];

  return <Box className="storage-manager">
    <input ref={fileInputRef} hidden type="file" multiple onChange={(event) => {
      void upload(event.currentTarget.files, false);
      event.currentTarget.value = "";
    }} />
    <input ref={folderInputRef} hidden type="file" multiple onChange={(event) => {
      void upload(event.currentTarget.files, true);
      event.currentTarget.value = "";
    }} />

    <Box className="storage-kind-switch">
      <Button variant={kind === "files" ? "contained" : "text"} startIcon={<FolderRounded />} onClick={() => changeKind("files")}>Files</Button>
      <Button variant={kind === "rms" ? "contained" : "text"} startIcon={<StorageRounded />} onClick={() => changeKind("rms")}>RMS</Button>
    </Box>

    {runningGame ? <Alert severity="warning">
      {runningGame.title} đang chạy. Có thể duyệt/tải xuống, nhưng hãy dừng game trước khi upload, tạo hoặc xóa dữ liệu.
    </Alert> : null}
    {notice ? <Alert severity={notice.severity} onClose={() => setNotice(null)}>{notice.message}</Alert> : null}

    <Box className="storage-breadcrumbs">
      <Button size="small" onClick={() => setPath("")}>{kind === "files" ? "Files" : "RMS"}</Button>
      {pathParts.map((part, index) => <Box className="storage-breadcrumb-part" key={`${part}-${index}`}>
        <ArrowForwardIosRounded />
        <Button size="small" onClick={() => setPath(pathParts.slice(0, index + 1).join("/"))}>{part}</Button>
      </Box>)}
    </Box>

    <Box className="storage-actions">
      <Button variant="outlined" startIcon={<UploadRounded />} disabled={busy || Boolean(runningGame)} onClick={() => fileInputRef.current?.click()}>Upload file</Button>
      <Button variant="outlined" startIcon={<UploadRounded />} disabled={busy || Boolean(runningGame)} onClick={() => folderInputRef.current?.click()}>Upload folder</Button>
      <Button variant="outlined" startIcon={<CreateNewFolderRounded />} disabled={busy || Boolean(runningGame)} onClick={() => setNewFolderOpen(true)}>Thư mục mới</Button>
      <Button variant="outlined" startIcon={<DownloadRounded />} disabled={busy} onClick={() => void downloadPath(path)}>Tải thư mục này</Button>
      <Button variant="text" disabled={busy || loading} onClick={() => void loadEntries()}>Làm mới</Button>
    </Box>

    <Box className="storage-list">
      {path ? <Box className="storage-entry storage-parent-entry" role="button" tabIndex={0} onClick={() => setPath(pathParts.slice(0, -1).join("/"))} onKeyDown={(event) => {
        if (event.key === "Enter" || event.key === " ") setPath(pathParts.slice(0, -1).join("/"));
      }}>
        <FolderRounded />
        <Box className="storage-entry-copy"><Typography>..</Typography><Typography variant="body2" color="text.secondary">Thư mục cha</Typography></Box>
      </Box> : null}
      {loading ? <Box className="storage-loading"><CircularProgress size={28} /></Box> : entries.length ? entries.map((entry) => {
        const suite = path === "" ? games.find((game) => String(game.suiteId) === entry.name) : undefined;
        const title = suite?.title ?? entry.name;
        const subtitle = suite
          ? `Suite ${entry.name}`
          : entry.isDirectory
            ? "Thư mục"
            : `${formatStorageSize(entry.size)}${entry.modifiedAt ? ` · ${new Date(entry.modifiedAt).toLocaleString()}` : ""}`;
        return <Box
          className="storage-entry"
          key={entry.path}
          role={entry.isDirectory ? "button" : undefined}
          tabIndex={entry.isDirectory ? 0 : undefined}
          onClick={() => { if (entry.isDirectory) setPath(entry.path); }}
          onKeyDown={(event) => {
            if (entry.isDirectory && (event.key === "Enter" || event.key === " ")) setPath(entry.path);
          }}
        >
          {entry.isDirectory ? <FolderRounded /> : <InsertDriveFileRounded />}
          <Box className="storage-entry-copy"><Typography noWrap>{title}</Typography><Typography variant="body2" color="text.secondary" noWrap>{subtitle}</Typography></Box>
          <Box className="storage-entry-actions">
            <IconButton aria-label={`Tải ${entry.name}`} disabled={busy} onClick={(event) => { event.stopPropagation(); void downloadPath(entry.path); }}><DownloadRounded /></IconButton>
            <IconButton aria-label={`Xóa ${entry.name}`} disabled={busy || Boolean(runningGame)} onClick={(event) => { event.stopPropagation(); setDeleteEntry(entry); }}><DeleteOutlineRounded /></IconButton>
          </Box>
        </Box>;
      }) : <Box className="storage-empty"><Typography color="text.secondary">Thư mục trống</Typography></Box>}
    </Box>

    <Dialog open={newFolderOpen} onClose={() => { if (!busy) setNewFolderOpen(false); }}>
      <DialogTitle>Thư mục mới</DialogTitle>
      <DialogContent><TextField autoFocus fullWidth margin="dense" label="Tên thư mục" value={newFolderName} onChange={(event) => setNewFolderName(event.target.value)} onKeyDown={(event) => { if (event.key === "Enter") void createFolder(); }} /></DialogContent>
      <DialogActions><Button disabled={busy} onClick={() => setNewFolderOpen(false)}>Hủy</Button><Button disabled={busy} onClick={() => void createFolder()}>Tạo</Button></DialogActions>
    </Dialog>

    <Dialog open={Boolean(deleteEntry)} onClose={() => { if (!busy) setDeleteEntry(null); }}>
      <DialogTitle>Xóa {deleteEntry?.name}?</DialogTitle>
      <DialogContent><DialogContentText>{deleteEntry?.isDirectory ? "Toàn bộ tệp bên trong thư mục này sẽ bị xóa khỏi IndexedDB." : "Tệp này sẽ bị xóa khỏi IndexedDB."}</DialogContentText></DialogContent>
      <DialogActions><Button disabled={busy} onClick={() => setDeleteEntry(null)}>Hủy</Button><Button color="error" disabled={busy} onClick={() => void confirmDelete()}>Xóa</Button></DialogActions>
    </Dialog>
  </Box>;
}

function GameAvatar({ game }: { game: GameEntry }) {
  return <Avatar
    src={game.iconDataUrl}
    variant="rounded"
    className="game-icon"
  >{game.iconDataUrl ? null : <GamepadRounded fontSize="small" />}</Avatar>;
}

function sectionTitle(value: string) {
  const normalized = value.trim().normalize("NFD").replace(/[\u0300-\u036f]/g, "");
  const first = normalized.charAt(0).toUpperCase();
  return /[A-Z]/.test(first) ? first : "#";
}

function LibraryView({
  games,
  searchText,
  onSearchTextChange,
  sort,
  descending,
  runtimePhase,
  runtimeMessage,
  onRetryRuntime,
  onImport,
  onLaunch,
  onContextMenu,
  onFiles
}: {
  games: GameEntry[];
  searchText: string;
  onSearchTextChange: (value: string) => void;
  sort: LibrarySort;
  descending: boolean;
  runtimePhase: RuntimePhase;
  runtimeMessage: string;
  onRetryRuntime: () => void;
  onImport: () => void;
  onLaunch: (game: GameEntry) => void;
  onContextMenu: (game: GameEntry, anchor: HTMLElement) => void;
  onFiles: (files: File[]) => void;
}) {
  const [dragging, setDragging] = useState(false);
  const longPressTimer = useRef<number | null>(null);

  const visibleGames = useMemo(() => {
    const query = searchText.trim().toLocaleLowerCase();
    return games
      .filter((game) => !query || [game.title, game.vendor, game.version, game.mainClass, game.fileName]
        .some((value) => value.toLocaleLowerCase().includes(query)))
      .sort((left, right) => {
        let result = 0;
        if (sort === "date") result = left.installedAt - right.installedAt;
        else if (sort === "vendor") result = (left.vendor || left.title).localeCompare(right.vendor || right.title, undefined, { sensitivity: "base" });
        else result = left.title.localeCompare(right.title, undefined, { sensitivity: "base" });
        return descending ? -result : result;
      });
  }, [descending, games, searchText, sort]);

  const sections = useMemo(() => {
    const grouped = new Map<string, GameEntry[]>();
    for (const game of visibleGames) {
      const title = sort === "date"
        ? new Date(game.installedAt).getFullYear().toString()
        : sectionTitle(sort === "vendor" ? (game.vendor || game.title) : game.title);
      grouped.set(title, [...(grouped.get(title) ?? []), game]);
    }
    return [...grouped.entries()];
  }, [sort, visibleGames]);

  return <Box
    className={`library-surface ${dragging ? "dragging" : ""}`}
    onDragEnter={(event) => { event.preventDefault(); setDragging(true); }}
    onDragOver={(event) => event.preventDefault()}
    onDragLeave={(event) => {
      if (!event.currentTarget.contains(event.relatedTarget as Node | null)) setDragging(false);
    }}
    onDrop={(event) => {
      event.preventDefault();
      setDragging(false);
      onFiles(Array.from(event.dataTransfer.files));
    }}
  >
    {games.length ? <>
      <Box className="library-search-wrap">
        <TextField
          fullWidth
          size="small"
          value={searchText}
          onChange={(event) => onSearchTextChange(event.target.value)}
          placeholder="Tìm ứng dụng"
          slotProps={{
            input: {
              startAdornment: <InputAdornment position="start"><SearchRounded fontSize="small" /></InputAdornment>,
              endAdornment: searchText ? <InputAdornment position="end">
                <IconButton size="small" aria-label="Xóa tìm kiếm" onClick={() => onSearchTextChange("")}><CloseRounded fontSize="small" /></IconButton>
              </InputAdornment> : undefined
            }
          }}
        />
      </Box>

      <Box className="inset-list">
        {sections.length ? sections.map(([title, sectionGames]) => <Box className="inset-section" key={title}>
          <Typography className="inset-section-header">{title}</Typography>
          <Box className="inset-section-body">
            {sectionGames.map((game) => <Box
              key={game.id}
              className="game-row"
              role="button"
              tabIndex={0}
              onClick={() => onLaunch(game)}
              onContextMenu={(event) => {
                event.preventDefault();
                onContextMenu(game, event.currentTarget);
              }}
              onPointerDown={(event) => {
                if (event.pointerType === "mouse") return;
                if (longPressTimer.current !== null) window.clearTimeout(longPressTimer.current);
                const anchor = event.currentTarget;
                longPressTimer.current = window.setTimeout(() => onContextMenu(game, anchor), 520);
              }}
              onPointerUp={() => {
                if (longPressTimer.current !== null) window.clearTimeout(longPressTimer.current);
                longPressTimer.current = null;
              }}
              onPointerCancel={() => {
                if (longPressTimer.current !== null) window.clearTimeout(longPressTimer.current);
                longPressTimer.current = null;
              }}
              onKeyDown={(event) => {
                if (event.key === "Enter" || event.key === " ") {
                  event.preventDefault();
                  onLaunch(game);
                }
              }}
            >
              <GameAvatar game={game} />
              <Box className="game-row-copy">
                <Box className="game-row-title-line">
                  <Typography className="game-row-title" noWrap>{game.title}</Typography>
                </Box>
                <Typography className="game-row-subtitle" noWrap>{game.vendor || game.fileName}</Typography>
              </Box>
              {game.version ? <Typography className="game-row-version" noWrap>{game.version}</Typography> : null}
            </Box>)}
          </Box>
          {title === sections.at(-1)?.[0] ? <Typography className="inset-section-footer">{games.length} {games.length === 1 ? "App" : "Apps"}</Typography> : null}
        </Box>) : <Box className="empty-filter-state">
          <Typography variant="subtitle1">Không có ứng dụng</Typography>
          <Typography variant="body2" color="text.secondary">Không có ứng dụng phù hợp bộ lọc hiện tại.</Typography>
        </Box>}
      </Box>
    </> : runtimePhase === "error" ? <Box className="empty-library">
      <ErrorOutlineRounded className="state-error-icon" />
      <Typography variant="h6">Không nạp được lõi phoneME</Typography>
      <Typography className="runtime-state-detail" color="text.secondary">{runtimeMessage}</Typography>
      <Button variant="contained" startIcon={<RestartAltRounded />} onClick={onRetryRuntime}>Thử lại</Button>
    </Box> : runtimePhase === "loading" || runtimePhase === "idle" ? <Box className="empty-library">
      <CircularProgress size={40} disableShrink />
      <Typography variant="h6">Đang nạp phoneME</Typography>
      <Typography className="runtime-state-detail" color="text.secondary">
        Lần đầu tải lõi WebAssembly có thể mất vài giây; các lần sau được cache.
      </Typography>
    </Box> : <Box className="empty-library">
      <GamepadRounded />
      <Typography variant="h6">Không có ứng dụng</Typography>
      <Typography color="text.secondary">Nhập file JAR J2ME để thêm vào thư viện.</Typography>
      <Button variant="contained" size="large" startIcon={<AddRounded />} onClick={onImport}>Nhập file JAR</Button>
      <Typography variant="body2" color="text.secondary">hoặc kéo thả file JAR vào đây</Typography>
    </Box>}

    {dragging ? <Box className="drop-overlay"><Typography variant="h6">Thả file JAR để nhập</Typography></Box> : null}
  </Box>;
}

function SettingSection({ title, children }: { title: string; children: React.ReactNode }) {
  return <Box className="form-section">
    <Typography className="form-section-header">{title}</Typography>
    <Box className="form-section-body">{children}</Box>
  </Box>;
}

function SettingRow({ title, value, children, className = "" }: {
  title: string;
  value?: string;
  children?: React.ReactNode;
  className?: string;
}) {
  return <Box className={`form-row ${className}`}>
    <Box className="form-row-copy">
      <Typography>{title}</Typography>
      {value ? <Typography variant="body2" color="text.secondary">{value}</Typography> : null}
    </Box>
    {children}
  </Box>;
}

function GameProfileEditor({ profile, onChange }: {
  profile: WebGameProfile;
  onChange: (profile: WebGameProfile) => void;
}) {
  const set = <K extends keyof WebGameProfile>(key: K, value: WebGameProfile[K]) => onChange(normalizeGameProfile({ ...profile, [key]: value }));
  const profileValue = `${profile.screenWidth}x${profile.screenHeight}`;

  return <Box className="profile-form">
    <SettingSection title="Hiển thị">
      <Box className="form-row screen-dimension-row">
        <TextField
          size="small"
          value={profile.screenWidth}
          onChange={(event) => set("screenWidth", Number.parseInt(event.target.value, 10) || profile.screenWidth)}
          slotProps={{ htmlInput: { inputMode: "numeric", "aria-label": "Chiều rộng" } }}
        />
        <Typography color="text.secondary">×</Typography>
        <TextField
          size="small"
          value={profile.screenHeight}
          onChange={(event) => set("screenHeight", Number.parseInt(event.target.value, 10) || profile.screenHeight)}
          slotProps={{ htmlInput: { inputMode: "numeric", "aria-label": "Chiều cao" } }}
        />
        <IconButton aria-label="Đổi chiều" onClick={() => onChange({ ...profile, screenWidth: profile.screenHeight, screenHeight: profile.screenWidth })}><SwapHorizRounded /></IconButton>
      </Box>
      <SettingRow title="Preset kích thước màn hình">
        <FormControl size="small" className="row-control">
          <Select
            displayEmpty
            value={SCREEN_PROFILES.some((item) => `${item.width}x${item.height}` === profileValue) ? profileValue : ""}
            onChange={(event) => {
              const [width, height] = String(event.target.value).split("x").map(Number);
              onChange({ ...profile, screenWidth: width, screenHeight: height });
            }}
          >
            <MenuItem value="" disabled>Tùy chỉnh</MenuItem>
            {SCREEN_PROFILES.map((item) => <MenuItem key={item.label} value={`${item.width}x${item.height}`}>{item.label}</MenuItem>)}
          </Select>
        </FormControl>
      </SettingRow>
      <FormControlLabel className="switch-row" labelPlacement="start" label="Giữ tỷ lệ Canvas" control={<Switch checked={profile.preserveAspectRatio} onChange={(_, checked) => set("preserveAspectRatio", checked)} />} />
      <Box className="slider-row">
        <Box className="slider-row-title"><Typography>Canvas scale</Typography><Typography color="text.secondary">{profile.scalePercent}%</Typography></Box>
        <Slider value={profile.scalePercent} min={10} max={300} step={1} onChange={(_, value) => set("scalePercent", value as number)} />
      </Box>
      <SettingRow title="Vị trí Canvas">
        <Select size="small" className="row-control" value={profile.screenGravity} onChange={(event) => set("screenGravity", event.target.value as ScreenGravity)}>
          <MenuItem value="left">Trái</MenuItem><MenuItem value="top">Trên</MenuItem><MenuItem value="center">Giữa</MenuItem><MenuItem value="right">Phải</MenuItem><MenuItem value="bottom">Dưới</MenuItem>
        </Select>
      </SettingRow>
      <SettingRow title="Kiểu scale Canvas">
        <Select size="small" className="row-control" value={profile.scaleType} onChange={(event) => set("scaleType", event.target.value as ScaleType)}>
          <MenuItem value="asIs">Giữ nguyên</MenuItem><MenuItem value="fit">Vừa cửa sổ</MenuItem><MenuItem value="fill">Lấp đầy cửa sổ</MenuItem>
        </Select>
      </SettingRow>
      <FormControlLabel className="switch-row" labelPlacement="start" label="Lọc ảnh" control={<Switch checked={profile.filtering} onChange={(_, checked) => set("filtering", checked)} />} />
      <FormControlLabel className="switch-row" labelPlacement="start" label="Buộc Canvas toàn màn hình" control={<Switch checked={profile.forceFullscreen} onChange={(_, checked) => set("forceFullscreen", checked)} />} />
      <FormControlLabel className="switch-row" labelPlacement="start" label="App Bar" control={<Switch checked={profile.showAppBar} onChange={(_, checked) => set("showAppBar", checked)} />} />
      <FormControlLabel className="switch-row" labelPlacement="start" label="Status Bar" control={<Switch checked={profile.showStatusBar} onChange={(_, checked) => set("showStatusBar", checked)} />} />
      <FormControlLabel className="switch-row" labelPlacement="start" label="Hiện FPS" control={<Switch checked={profile.showFPS} onChange={(_, checked) => set("showFPS", checked)} />} />
    </SettingSection>

    <SettingSection title="Bộ nhớ">
      <SettingRow title="Java heap" value={`${profile.heapSizeMegabytes} MiB`}>
        <Select size="small" className="row-control" value={profile.heapSizeMegabytes} onChange={(event) => set("heapSizeMegabytes", Number(event.target.value))}>
          {HEAP_PRESETS.map((size) => <MenuItem key={size} value={size}>{size} MiB</MenuItem>)}
        </Select>
      </SettingRow>
    </SettingSection>

    <SettingSection title="Phông chữ">
      <Box className="font-size-row">
        <TextField size="small" label="Nhỏ" value={profile.fontSmall} onChange={(event) => set("fontSmall", Number.parseInt(event.target.value, 10) || profile.fontSmall)} />
        <TextField size="small" label="Vừa" value={profile.fontMedium} onChange={(event) => set("fontMedium", Number.parseInt(event.target.value, 10) || profile.fontMedium)} />
        <TextField size="small" label="Lớn" value={profile.fontLarge} onChange={(event) => set("fontLarge", Number.parseInt(event.target.value, 10) || profile.fontLarge)} />
      </Box>
      <FormControlLabel className="switch-row" labelPlacement="start" label="Dùng Dynamic Type scaling" control={<Switch checked={profile.fontValuesAreScaledPixels} onChange={(_, checked) => set("fontValuesAreScaledPixels", checked)} />} />
    </SettingSection>

    <SettingSection title="Điều khiển">
      <FormControlLabel className="switch-row" labelPlacement="start" label="Cảm ứng" control={<Switch checked={profile.touchInput} onChange={(_, checked) => set("touchInput", checked)} />} />
      <SettingRow title="Layout phím J2ME">
        <Select size="small" className="row-control" value={profile.keyLayout} onChange={(event) => set("keyLayout", event.target.value as KeyLayout)}>
          <MenuItem value="nokiaSE">Nokia/SE</MenuItem><MenuItem value="siemens">Siemens</MenuItem><MenuItem value="motorola">Motorola</MenuItem><MenuItem value="custom">Tùy chỉnh</MenuItem>
        </Select>
      </SettingRow>
      <FormControlLabel className="switch-row" labelPlacement="start" label="Bàn phím ảo" control={<Switch checked={profile.showVirtualKeyboard} onChange={(_, checked) => set("showVirtualKeyboard", checked)} />} />
      <SettingRow title="Layout bàn phím ảo">
        <Select size="small" className="row-control" disabled={!profile.showVirtualKeyboard} value={profile.virtualKeyboardType} onChange={(event) => set("virtualKeyboardType", event.target.value as VirtualKeyboardType)}>
          <MenuItem value="phone">Phone</MenuItem><MenuItem value="phoneArrows">Phone (arrows)</MenuItem><MenuItem value="numbersArrows">Numbers & arrows</MenuItem><MenuItem value="arrowsNumbers">Arrows & numbers</MenuItem><MenuItem value="numbers">Numbers</MenuItem><MenuItem value="arrows">Arrows</MenuItem>
        </Select>
      </SettingRow>
      <SettingRow title="Hình dạng nút">
        <Select size="small" className="row-control" disabled={!profile.showVirtualKeyboard} value={profile.buttonShape} onChange={(event) => set("buttonShape", event.target.value as ButtonShape)}>
          <MenuItem value="oval">Oval</MenuItem><MenuItem value="rectangle">Chữ nhật</MenuItem><MenuItem value="roundedRectangle">Chữ nhật bo góc</MenuItem>
        </Select>
      </SettingRow>
      <FormControlLabel className="switch-row" labelPlacement="start" label="Rung phản hồi" control={<Switch checked={profile.hapticFeedback} disabled={!profile.showVirtualKeyboard} onChange={(_, checked) => set("hapticFeedback", checked)} />} />
      <Box className="slider-row disabled-aware">
        <Box className="slider-row-title"><Typography>Độ trong suốt bàn phím</Typography><Typography color="text.secondary">{Math.round(profile.keyboardOpacity * 100)}%</Typography></Box>
        <Slider disabled={!profile.showVirtualKeyboard} value={profile.keyboardOpacity} min={0.05} max={1} step={0.05} onChange={(_, value) => set("keyboardOpacity", value as number)} />
      </Box>
      <FormControlLabel className="switch-row" labelPlacement="start" label="Giữ phím ngoài màn hình rõ hoàn toàn" control={<Switch checked={profile.forceOpacityForOffscreenKeys} disabled={!profile.showVirtualKeyboard} onChange={(_, checked) => set("forceOpacityForOffscreenKeys", checked)} />} />
      <SettingRow title="Tự ẩn sau" value="ms"><TextField className="numeric-row-field" size="small" disabled={!profile.showVirtualKeyboard} value={profile.keyboardHideDelayMilliseconds} onChange={(event) => set("keyboardHideDelayMilliseconds", Number.parseInt(event.target.value, 10) || 0)} /></SettingRow>
    </SettingSection>
  </Box>;
}

function SettingsView({ settings, onChange, runtimeReady }: {
  settings: AppSettings;
  onChange: (settings: AppSettings) => void;
  runtimeReady: boolean;
}) {
  return <Box className="profile-form global-settings-form">
    <SettingSection title="Ngôn ngữ">
      <SettingRow title="Ngôn ngữ">
        <Select size="small" className="row-control" value={settings.language} onChange={(event) => onChange({ ...settings, language: event.target.value as AppSettings["language"] })}>
          <MenuItem value="system">Hệ thống</MenuItem><MenuItem value="vi">Tiếng Việt</MenuItem><MenuItem value="en">English</MenuItem>
        </Select>
      </SettingRow>
    </SettingSection>
    <SettingSection title="Giao diện">
      <SettingRow title="Giao diện">
        <Select size="small" className="row-control" value={settings.theme} onChange={(event) => onChange({ ...settings, theme: event.target.value as ThemePreference })}>
          <MenuItem value="system">Hệ thống</MenuItem><MenuItem value="light">Sáng</MenuItem><MenuItem value="dark">Tối</MenuItem>
        </Select>
      </SettingRow>
    </SettingSection>
    <SettingSection title="Dịch thuật">
      <SettingRow title="Dịch vụ dịch">
        <Select size="small" className="row-control" value={settings.translationProvider} onChange={(event) => onChange({ ...settings, translationProvider: event.target.value as AppSettings["translationProvider"] })}>
          <MenuItem value="bing">Bing Translator</MenuItem><MenuItem value="google">Google Translate</MenuItem><MenuItem value="automatic">Tự động</MenuItem>
        </Select>
      </SettingRow>
    </SettingSection>
    <SettingSection title="JIT">
      <SettingRow title="Trạng thái JIT" value={runtimeReady ? "WebAssembly runtime sẵn sàng" : "Đang khởi động"} />
    </SettingSection>
    <SettingSection title="Trình chơi">
      <FormControlLabel className="switch-row" labelPlacement="start" label="App Bar" control={<Switch checked={settings.enableActionBar} onChange={(_, checked) => onChange({ ...settings, enableActionBar: checked })} />} />
      <FormControlLabel className="switch-row" labelPlacement="start" label="Status Bar" control={<Switch checked={settings.enableStatusBar} onChange={(_, checked) => onChange({ ...settings, enableStatusBar: checked })} />} />
      <FormControlLabel className="switch-row" labelPlacement="start" label="Giữ màn hình sáng" control={<Switch checked={settings.keepScreenOn} onChange={(_, checked) => onChange({ ...settings, keepScreenOn: checked })} />} />
    </SettingSection>
    <SettingSection title="Mạng">
      <Box className="proxy-row">
        <Typography>TCP proxy (websockify)</Typography>
        <TextField fullWidth size="small" placeholder="wss://127.0.0.1:38473" value={settings.websocketProxyUrl} onChange={(event) => onChange({ ...settings, websocketProxyUrl: event.target.value.trim() })} />
        <Typography variant="body2" color="text.secondary">
          HTTP/HTTPS dùng trực tiếp mạng của trình duyệt. socket:// được chuyển qua websockify; tải lại trang để áp dụng proxy mới.
        </Typography>
      </Box>
    </SettingSection>
    <SettingSection title="Lưu trữ">
      <Alert severity="info">Thư viện, RMS và filesystem được lưu trong IndexedDB của trình duyệt.</Alert>
    </SettingSection>
  </Box>;
}

export default function App() {
  const prefersDark = useMediaQuery("(prefers-color-scheme: dark)");
  const runtimeRef = useRef(new PhoneMEWebRuntime());
  const importInputRef = useRef<HTMLInputElement>(null);
  const [settings, setSettings] = useState<AppSettings>(() => {
    const legacy = readJson<Partial<AppSettings>>(LEGACY_SETTINGS_KEY, {});
    const current = readJson<Partial<AppSettings>>(SETTINGS_KEY, {});
    const merged = { ...DEFAULT_SETTINGS, ...legacy, ...current };
    return {
      ...merged,
      websocketProxyUrl: normalizeWebsocketProxyUrl(merged.websocketProxyUrl)
    };
  });
  const [libraryPreferences, setLibraryPreferences] = useState<LibraryPreferences>(() => ({ ...DEFAULT_LIBRARY_PREFERENCES, ...readJson<Partial<LibraryPreferences>>(LIBRARY_PREFS_KEY, {}) }));
  const [games, setGames] = useState<GameEntry[]>(() => readJson<GameEntry[]>(GAMES_KEY, []));
  const [profiles, setProfiles] = useState<Record<string, WebGameProfile>>(() => {
    const stored = readJson<Record<string, Partial<WebGameProfile>>>(PROFILES_KEY, {});
    return Object.fromEntries(Object.entries(stored).map(([id, value]) => [id, normalizeGameProfile(value)]));
  });
  const [view, setView] = useState<ViewId>("library");
  const [activeGame, setActiveGame] = useState<GameEntry | null>(null);
  const [draftProfile, setDraftProfile] = useState<WebGameProfile>(DEFAULT_GAME_PROFILE);
  const [runtimeSnapshot, setRuntimeSnapshot] = useState<RuntimeSnapshot>(EMPTY_SNAPSHOT);
  const [installing, setInstalling] = useState(false);
  const [searchText, setSearchText] = useState("");
  const [libraryMenuAnchor, setLibraryMenuAnchor] = useState<HTMLElement | null>(null);
  const [sortMenuAnchor, setSortMenuAnchor] = useState<HTMLElement | null>(null);
  const [orderMenuAnchor, setOrderMenuAnchor] = useState<HTMLElement | null>(null);
  const [configMenuAnchor, setConfigMenuAnchor] = useState<HTMLElement | null>(null);
  const [contextMenuAnchor, setContextMenuAnchor] = useState<HTMLElement | null>(null);
  const [contextGame, setContextGame] = useState<GameEntry | null>(null);
  const [renameTarget, setRenameTarget] = useState<GameEntry | null>(null);
  const [renameText, setRenameText] = useState("");
  const [deleteTarget, setDeleteTarget] = useState<GameEntry | null>(null);
  const [deleteData, setDeleteData] = useState(true);
  const [showProfiles, setShowProfiles] = useState(false);
  const [storageInitialKind, setStorageInitialKind] = useState<ManagedStorageKind>("files");
  const [snackbar, setSnackbar] = useState<{ message: string; severity: "success" | "error" | "info" } | null>(null);
  const [pwaUpdateVersion, setPwaUpdateVersion] = useState<string | null>(null);
  const [pwaUpdating, setPwaUpdating] = useState(false);
  const [logs, setLogs] = useState<string[]>([]);
  const [initAttempt, setInitAttempt] = useState(0);

  const paletteMode = settings.theme === "system" ? (prefersDark ? "dark" : "light") : settings.theme;
  const theme = useMemo(() => createPhoneMETheme(paletteMode), [paletteMode]);
  const runtime = runtimeRef.current;

  // Keep the PWA status-bar tint locked to the appbar surface (MD3 surface-container)
  // and switch our design tokens on the same attribute. The static media-scoped metas
  // in index.html cover first paint; once React is up this single meta wins on Android.
  useEffect(() => {
    document.documentElement.dataset.colorMode = paletteMode;
    document.head.querySelectorAll('meta[name="theme-color"]').forEach((meta) => meta.remove());
    const meta = document.createElement("meta");
    meta.setAttribute("name", "theme-color");
    meta.setAttribute("content", appbarThemeColor(paletteMode));
    document.head.appendChild(meta);
  }, [paletteMode]);

  useEffect(() => {
    const onUpdateReady = (event: Event) => {
      const detail = (event as CustomEvent<PwaUpdateReadyDetail>).detail;
      if (detail?.version) setPwaUpdateVersion(detail.version);
    };
    window.addEventListener(PWA_UPDATE_READY_EVENT, onUpdateReady);
    const pendingVersion = getPendingPwaUpdateVersion();
    if (pendingVersion) setPwaUpdateVersion(pendingVersion);
    void checkForPwaUpdate();
    return () => window.removeEventListener(PWA_UPDATE_READY_EVENT, onUpdateReady);
  }, []);

  const installPwaUpdate = useCallback(() => {
    if (!pwaUpdateVersion || pwaUpdating) return;
    setPwaUpdating(true);
    void applyPwaUpdate(pwaUpdateVersion).catch((error) => {
      setPwaUpdating(false);
      setSnackbar({ message: error instanceof Error ? error.message : String(error), severity: "error" });
    });
  }, [pwaUpdateVersion, pwaUpdating]);

  const saveSettings = useCallback((next: AppSettings) => {
    setSettings(next);
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(next));
  }, []);

  const saveGames = useCallback((next: GameEntry[]) => {
    setGames(next);
    localStorage.setItem(GAMES_KEY, JSON.stringify(next));
  }, []);

  const saveLibraryPreferences = useCallback((next: LibraryPreferences) => {
    setLibraryPreferences(next);
    localStorage.setItem(LIBRARY_PREFS_KEY, JSON.stringify(next));
  }, []);

  const saveProfile = useCallback((game: GameEntry, profile: WebGameProfile) => {
    const nextProfile = normalizeGameProfile(profile);
    setProfiles((current) => {
      const next = { ...current, [game.id]: nextProfile };
      localStorage.setItem(PROFILES_KEY, JSON.stringify(next));
      return next;
    });
    return nextProfile;
  }, []);

  useEffect(() => {
    let active = true;
    setRuntimeSnapshot(EMPTY_SNAPSHOT);
    const options = {
      websocketProxyUrl: settings.websocketProxyUrl || undefined,
      onLog: (line: string, error: boolean) => {
        if (!active) return;
        setLogs((current) => [...current.slice(-199), `${error ? "ERR" : "LOG"} ${line}`]);
      }
    };
    void (async () => {
      let lastError: unknown;
      for (let attempt = 0; attempt < 2; attempt += 1) {
        try {
          await runtime.initialize(options);
          if (!active) return;
          setRuntimeSnapshot({ ...EMPTY_SNAPSHOT, phase: "ready", message: "phoneME Web đã sẵn sàng" });
          return;
        } catch (error) {
          lastError = error;
          if (!active) return;
          if (attempt === 0) await new Promise((resolve) => window.setTimeout(resolve, 120));
        }
      }
      if (!active) return;
      const message = lastError instanceof Error ? lastError.message : String(lastError);
      setRuntimeSnapshot({ ...EMPTY_SNAPSHOT, phase: "error", message });
      setSnackbar({ message, severity: "error" });
    })();
    const flush = () => { void runtime.flushStorage().catch(() => undefined); };
    const flushWhenHidden = () => {
      if (document.visibilityState === "hidden") flush();
    };
    window.addEventListener("pagehide", flush);
    window.addEventListener("beforeunload", flush);
    document.addEventListener("visibilitychange", flushWhenHidden);
    return () => {
      active = false;
      window.removeEventListener("pagehide", flush);
      window.removeEventListener("beforeunload", flush);
      document.removeEventListener("visibilitychange", flushWhenHidden);
      flush();
      runtime.dispose();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [initAttempt, runtime]);

  useEffect(() => {
    if (!settings.keepScreenOn || !("wakeLock" in navigator)) return;
    let released = false;
    let lock: { release: () => Promise<void> } | null = null;
    void (navigator as Navigator & { wakeLock?: { request: (type: "screen") => Promise<{ release: () => Promise<void> }> } }).wakeLock?.request("screen").then((value) => {
      if (released) void value.release();
      else lock = value;
    }).catch(() => undefined);
    return () => {
      released = true;
      if (lock) void lock.release();
    };
  }, [settings.keepScreenOn]);

  const importFiles = useCallback(async (files: File[]) => {
    const jars = files.filter((file) => file.name.toLowerCase().endsWith(".jar"));
    if (!jars.length) {
      setSnackbar({ message: "Vui lòng chọn file .jar", severity: "error" });
      return;
    }
    setInstalling(true);
    const installed: GameEntry[] = [];
    try {
      for (const file of jars) {
        const metadata = await readJarMetadata(file);
        installed.push(await runtime.installJar(file, metadata));
      }
      const next = [...games];
      for (const installedGame of installed) {
        const existingIndex = next.findIndex((game) => game.suiteId === installedGame.suiteId);
        if (existingIndex < 0) {
          next.push(installedGame);
          continue;
        }

        // SuiteStore deliberately keeps the same suiteId when an installed
        // MIDlet is upgraded. Preserve our UI/profile identity, but replace all
        // manifest/launch metadata with the newly installed JAR. Keeping the
        // older entry here could leave mainClass/version/title stale and make
        // an otherwise successful update impossible to launch.
        next[existingIndex] = {
          ...installedGame,
          id: next[existingIndex].id
        };
      }
      saveGames(next);
      setSnackbar({ message: `Đã cài ${installed.length} ứng dụng`, severity: "success" });
    } catch (error) {
      setSnackbar({ message: error instanceof Error ? error.message : String(error), severity: "error" });
      if (installed.length) {
        const next = [...games];
        for (const installedGame of installed) {
          const existingIndex = next.findIndex((game) => game.suiteId === installedGame.suiteId);
          if (existingIndex < 0) next.push(installedGame);
          else next[existingIndex] = { ...installedGame, id: next[existingIndex].id };
        }
        saveGames(next);
      }
    } finally {
      // JAR inspection/installation can leave a large Wasm high-water mark.
      // Start the next MIDlet in a fresh instance after all selected JARs have
      // been persisted, rather than reusing the installer process.
      try {
        await runtime.recycleIdleRuntime();
      } catch {
        // installJar already waits for the durable IDBFS flush. Recycling is a
        // memory/state hygiene optimization and must not turn a good install
        // into a false UI failure if a final redundant flush is interrupted.
      }
      setInstalling(false);
    }
  }, [games, runtime, saveGames]);

  const profileFor = useCallback((game: GameEntry) => normalizeGameProfile(profiles[game.id] ?? {
    showAppBar: settings.enableActionBar,
    showStatusBar: settings.enableStatusBar
  }), [profiles, settings.enableActionBar, settings.enableStatusBar]);

  const openGame = useCallback((game: GameEntry) => {
    setActiveGame(game);
    const profile = profileFor(game);
    setDraftProfile(profile);
    if (runtime.activeGame?.id === game.id) {
      setView("emulator");
      return;
    }
    setView(profiles[game.id] ? "emulator" : "configure");
  }, [profileFor, profiles, runtime]);

  const openGameSettings = useCallback((game: GameEntry) => {
    setActiveGame(game);
    setDraftProfile(profileFor(game));
    setContextMenuAnchor(null);
    setContextGame(null);
    setView("configure");
  }, [profileFor]);

  const startGame = useCallback(() => {
    if (!activeGame) return;
    const saved = saveProfile(activeGame, draftProfile);
    setDraftProfile(saved);
    setView("emulator");
  }, [activeGame, draftProfile, saveProfile]);

  const beginRename = (game: GameEntry) => {
    setRenameTarget(game);
    setRenameText(game.title);
    setContextMenuAnchor(null);
    setContextGame(null);
  };

  const confirmRename = () => {
    if (!renameTarget) return;
    const title = renameText.trim();
    if (title) saveGames(games.map((game) => game.id === renameTarget.id ? { ...game, title } : game));
    setRenameTarget(null);
  };

  const confirmDelete = useCallback(async (removeData: boolean) => {
    if (!deleteTarget) return;
    try {
      await runtime.uninstall(deleteTarget, removeData);
      saveGames(games.filter((game) => game.id !== deleteTarget.id));
      setProfiles((current) => {
        const next = { ...current };
        delete next[deleteTarget.id];
        localStorage.setItem(PROFILES_KEY, JSON.stringify(next));
        return next;
      });
      if (activeGame?.id === deleteTarget.id) {
        setActiveGame(null);
        setView("library");
      }
      setSnackbar({ message: `Đã gỡ ${deleteTarget.title}`, severity: "success" });
    } catch (error) {
      setSnackbar({ message: error instanceof Error ? error.message : String(error), severity: "error" });
    } finally {
      setDeleteTarget(null);
    }
  }, [activeGame?.id, deleteTarget, games, runtime, saveGames]);

  const showingEmulator = view === "emulator" && Boolean(activeGame);

  return <ThemeProvider theme={theme}>
    <Box
      className={`app-root ${draftProfile.filtering ? "smooth" : "pixelated"}`}
      data-runtime-phase={runtimeSnapshot.phase}
      data-runtime-memory={Math.max(0, Math.round(runtimeSnapshot.usedMemory))}
    >
      <input
        ref={importInputRef}
        hidden
        type="file"
        accept=".jar,application/java-archive,application/zip"
        multiple
        onChange={(event) => {
          void importFiles(Array.from(event.target.files ?? []));
          event.currentTarget.value = "";
        }}
      />

      {!showingEmulator ? <AppBar position="sticky" elevation={0} className="native-appbar">
        <Toolbar>
          {view === "library" ? <IconButton color="inherit" aria-label="Nhập JAR" disabled={installing} onClick={() => importInputRef.current?.click()}>
            {installing ? <CircularProgress size={22} color="inherit" /> : <AddRounded />}
          </IconButton> : view === "configure" ? <Button color="inherit" className="bar-text-action" onClick={() => { setActiveGame(null); setView("library"); }}>Hủy</Button> : <Box className="bar-leading-spacer" />}

          <Typography className="native-appbar-title" variant="h6" noWrap>
            {view === "configure" ? activeGame?.title : view === "settings" ? "Cài đặt" : view === "storage" ? "File & RMS" : "Thư viện"}
          </Typography>

          {view === "library" ? <>
            <IconButton className="ellipsis-circle-button" color="inherit" aria-label="Thêm tùy chọn" onClick={(event) => setLibraryMenuAnchor(event.currentTarget)}><MoreHorizRounded /></IconButton>
          </> : view === "configure" ? <>
            <IconButton color="inherit" aria-label="Thêm tùy chọn" onClick={(event) => setConfigMenuAnchor(event.currentTarget)}><MoreHorizRounded /></IconButton>
            <Button color="inherit" className="bar-text-action primary" onClick={startGame}>Bắt đầu</Button>
          </> : <Button color="inherit" className="bar-text-action primary" onClick={() => setView("library")}>Xong</Button>}
        </Toolbar>
      </AppBar> : null}

      <Box component="main" className={showingEmulator ? "emulator-page" : "page-surface"}>
        {showingEmulator && activeGame ? <EmulatorScreen
          runtime={runtime}
          game={activeGame}
          profile={draftProfile}
          translationProvider={settings.translationProvider}
          onProfileChange={(profile) => {
            setDraftProfile(profile);
            saveProfile(activeGame, profile);
          }}
          onHide={() => {
            setRuntimeSnapshot({ ...EMPTY_SNAPSHOT, phase: "ready", message: "phoneME Web đã sẵn sàng" });
            setActiveGame(null);
            setView("library");
          }}
          onStop={() => {
            runtime.endAppSession();
            setRuntimeSnapshot({ ...EMPTY_SNAPSHOT, phase: "ready", message: "phoneME Web đã sẵn sàng" });
            setActiveGame(null);
            setView("library");
          }}
          onSnapshot={setRuntimeSnapshot}
        /> : view === "configure" && activeGame ? <GameProfileEditor profile={draftProfile} onChange={setDraftProfile} /> : view === "settings" ? <SettingsView settings={settings} onChange={saveSettings} runtimeReady={runtime.ready} /> : view === "storage" ? <StorageManagerView runtime={runtime} games={games} initialKind={storageInitialKind} /> : <LibraryView
          games={games}
          searchText={searchText}
          onSearchTextChange={setSearchText}
          sort={libraryPreferences.sort}
          descending={libraryPreferences.descending}
          runtimePhase={runtimeSnapshot.phase}
          runtimeMessage={runtimeSnapshot.message}
          onRetryRuntime={() => setInitAttempt((attempt) => attempt + 1)}
          onImport={() => importInputRef.current?.click()}
          onLaunch={openGame}
          onContextMenu={(game, anchor) => { setContextGame(game); setContextMenuAnchor(anchor); }}
          onFiles={(files) => void importFiles(files)}
        />}
      </Box>

      <Menu className="native-menu" anchorEl={libraryMenuAnchor} open={Boolean(libraryMenuAnchor)} onClose={() => { setLibraryMenuAnchor(null); setSortMenuAnchor(null); setOrderMenuAnchor(null); }}>
        <MenuItem onClick={(event) => setSortMenuAnchor(event.currentTarget)}>
          <SortByAlphaRounded fontSize="small" /><span>Sắp xếp theo</span><ArrowForwardIosRounded className="submenu-chevron" />
        </MenuItem>
        <MenuItem onClick={(event) => setOrderMenuAnchor(event.currentTarget)}>
          {libraryPreferences.descending ? <ArrowDownwardRounded fontSize="small" /> : <ArrowUpwardRounded fontSize="small" />}
          <span>Thứ tự</span><ArrowForwardIosRounded className="submenu-chevron" />
        </MenuItem>
        <Divider />
        <MenuItem onClick={() => { setStorageInitialKind("files"); setLibraryMenuAnchor(null); setView("storage"); }}><StorageRounded fontSize="small" /><span>File & RMS</span></MenuItem>
        <MenuItem onClick={() => { setLibraryMenuAnchor(null); setView("settings"); }}><SettingsRounded fontSize="small" /><span>Cài đặt</span></MenuItem>
        <MenuItem onClick={() => { setLibraryMenuAnchor(null); setShowProfiles(true); }}><TuneRounded fontSize="small" /><span>Profiles</span></MenuItem>
      </Menu>

      <Menu className="native-menu" anchorEl={sortMenuAnchor} open={Boolean(sortMenuAnchor)} onClose={() => setSortMenuAnchor(null)} anchorOrigin={{ vertical: "top", horizontal: "right" }} transformOrigin={{ vertical: "top", horizontal: "left" }}>
        <MenuItem onClick={() => { saveLibraryPreferences({ ...libraryPreferences, sort: "name" }); setSortMenuAnchor(null); }}>
          <SortByAlphaRounded fontSize="small" /><span>Tên</span>{libraryPreferences.sort === "name" ? <CheckRounded className="menu-trailing-check" /> : null}
        </MenuItem>
        <MenuItem onClick={() => { saveLibraryPreferences({ ...libraryPreferences, sort: "date" }); setSortMenuAnchor(null); }}>
          <CalendarMonthRounded fontSize="small" /><span>Ngày</span>{libraryPreferences.sort === "date" ? <CheckRounded className="menu-trailing-check" /> : null}
        </MenuItem>
        <MenuItem onClick={() => { saveLibraryPreferences({ ...libraryPreferences, sort: "vendor" }); setSortMenuAnchor(null); }}>
          <BusinessRounded fontSize="small" /><span>Nhà phát hành</span>{libraryPreferences.sort === "vendor" ? <CheckRounded className="menu-trailing-check" /> : null}
        </MenuItem>
      </Menu>

      <Menu className="native-menu" anchorEl={orderMenuAnchor} open={Boolean(orderMenuAnchor)} onClose={() => setOrderMenuAnchor(null)} anchorOrigin={{ vertical: "top", horizontal: "right" }} transformOrigin={{ vertical: "top", horizontal: "left" }}>
        <MenuItem onClick={() => { saveLibraryPreferences({ ...libraryPreferences, descending: false }); setOrderMenuAnchor(null); }}>
          <ArrowUpwardRounded fontSize="small" /><span>Tăng dần</span>{!libraryPreferences.descending ? <CheckRounded className="menu-trailing-check" /> : null}
        </MenuItem>
        <MenuItem onClick={() => { saveLibraryPreferences({ ...libraryPreferences, descending: true }); setOrderMenuAnchor(null); }}>
          <ArrowDownwardRounded fontSize="small" /><span>Giảm dần</span>{libraryPreferences.descending ? <CheckRounded className="menu-trailing-check" /> : null}
        </MenuItem>
      </Menu>

      <Menu className="native-menu" anchorEl={configMenuAnchor} open={Boolean(configMenuAnchor)} onClose={() => setConfigMenuAnchor(null)}>
        <MenuItem onClick={() => { setDraftProfile({ ...DEFAULT_GAME_PROFILE, showAppBar: settings.enableActionBar, showStatusBar: settings.enableStatusBar }); setConfigMenuAnchor(null); }}><RestartAltRounded fontSize="small" /><span>Đặt lại cài đặt</span></MenuItem>
        <MenuItem onClick={() => { setDraftProfile((profile) => ({ ...profile, keyLayout: "nokiaSE", virtualKeyboardType: "arrowsNumbers" })); setConfigMenuAnchor(null); }}><KeyboardRounded fontSize="small" /><span>Đặt lại layout phím</span></MenuItem>
      </Menu>

      <Menu className="native-menu" anchorEl={contextMenuAnchor} open={Boolean(contextMenuAnchor)} onClose={() => { setContextMenuAnchor(null); setContextGame(null); }}>
        {contextGame ? <>
          {runtime.activeGame?.id === contextGame.id ? <MenuItem className="destructive-menu-item" onClick={() => {
            const game = contextGame;
            setContextMenuAnchor(null);
            setContextGame(null);
            runtime.endAppSession();
            if (activeGame?.id === game.id) setActiveGame(null);
            setRuntimeSnapshot({ ...EMPTY_SNAPSHOT, phase: "ready", message: "phoneME Web đã sẵn sàng" });
          }}><StopCircleRounded fontSize="small" /><span>Dừng</span></MenuItem> : null}
          <MenuItem onClick={() => { setStorageInitialKind("rms"); setContextMenuAnchor(null); setContextGame(null); setView("storage"); }}><StorageRounded fontSize="small" /><span>Quản lý RMS</span></MenuItem>
          <MenuItem onClick={() => { setStorageInitialKind("files"); setContextMenuAnchor(null); setContextGame(null); setView("storage"); }}><FolderRounded fontSize="small" /><span>Quản lý Files</span></MenuItem>
          <Divider />
          <MenuItem onClick={() => beginRename(contextGame)}><EditRounded fontSize="small" /><span>Đổi tên</span></MenuItem>
          <MenuItem onClick={() => openGameSettings(contextGame)}><SettingsRounded fontSize="small" /><span>Cài đặt</span></MenuItem>
          <MenuItem className="destructive-menu-item" onClick={() => { setDeleteTarget(contextGame); setContextMenuAnchor(null); setContextGame(null); }}><DeleteOutlineRounded fontSize="small" /><span>Gỡ cài đặt</span></MenuItem>
        </> : null}
      </Menu>

      <Dialog open={Boolean(renameTarget)} onClose={() => setRenameTarget(null)}>
        <DialogTitle>Đổi tên</DialogTitle>
        <DialogContent><TextField autoFocus fullWidth margin="dense" label="Tên ứng dụng" value={renameText} onChange={(event) => setRenameText(event.target.value)} onKeyDown={(event) => { if (event.key === "Enter") confirmRename(); }} /></DialogContent>
        <DialogActions><Button onClick={() => setRenameTarget(null)}>Hủy</Button><Button onClick={confirmRename}>OK</Button></DialogActions>
      </Dialog>

      <Dialog open={Boolean(deleteTarget)} onClose={() => setDeleteTarget(null)}>
        <DialogTitle>Gỡ {deleteTarget?.title}?</DialogTitle>
        <DialogContent>
          <DialogContentText>Giữ dữ liệu để khôi phục save và cài đặt khi nhập lại cùng JAR, hoặc xóa toàn bộ RMS, file và cài đặt ứng dụng vĩnh viễn.</DialogContentText>
        </DialogContent>
        <DialogActions className="stacked-dialog-actions">
          <Button color="error" onClick={() => void confirmDelete(true)}>Gỡ ứng dụng & xóa dữ liệu</Button>
          <Button color="error" onClick={() => void confirmDelete(false)}>Chỉ gỡ ứng dụng</Button>
          <Button onClick={() => setDeleteTarget(null)}>Hủy</Button>
        </DialogActions>
      </Dialog>

      <Dialog open={showProfiles} onClose={() => setShowProfiles(false)} fullWidth maxWidth="sm">
        <DialogTitle>Profiles</DialogTitle>
        <DialogContent className="profiles-dialog-content">
          {games.filter((game) => Boolean(profiles[game.id])).length ? games.filter((game) => Boolean(profiles[game.id])).map((game) => <Button
            key={game.id}
            className="profile-dialog-row"
            onClick={() => {
              setShowProfiles(false);
              openGameSettings(game);
            }}
          >
            <GameAvatar game={game} />
            <Box className="profile-dialog-copy">
              <Typography noWrap>{game.title}</Typography>
              <Typography variant="body2" color="text.secondary" noWrap>{profiles[game.id].screenWidth} × {profiles[game.id].screenHeight} · {profiles[game.id].virtualKeyboardType}</Typography>
            </Box>
          </Button>) : <Typography color="text.secondary">Chưa có profile đã lưu.</Typography>}
        </DialogContent>
        <DialogActions><Button onClick={() => setShowProfiles(false)}>Xong</Button></DialogActions>
      </Dialog>

      <Snackbar open={Boolean(snackbar)} autoHideDuration={4200} onClose={() => setSnackbar(null)}>
        {snackbar ? <Alert severity={snackbar.severity} variant="filled" onClose={() => setSnackbar(null)}>{snackbar.message}</Alert> : undefined}
      </Snackbar>

      <Snackbar open={Boolean(pwaUpdateVersion)} anchorOrigin={{ vertical: "bottom", horizontal: "center" }}>
        <Alert
          severity="info"
          variant="filled"
          action={<Button color="inherit" size="small" disabled={pwaUpdating} onClick={installPwaUpdate}>{pwaUpdating ? "Đang cập nhật…" : "Cập nhật"}</Button>}
        >
          Có bản cập nhật phoneME Web mới.
        </Alert>
      </Snackbar>

      <Box className="sr-only" aria-live="polite">{runtimeSnapshot.message} {logs.at(-1)}</Box>
    </Box>
  </ThemeProvider>;
}
