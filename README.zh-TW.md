# Virtua Racing MD Enhanced

[English](README.md) | 繁體中文

以模擬器為基礎，執行原版 Mega Drive / Genesis《Virtua Racing》並強化畫面的專案。
核心採用開源模擬器 Genesis Plus GX。
使用 Claude Code Opus 5.5 協助開發。

此版本為懷舊與研究用，若想要更完美的街機版體驗，推薦 Switch 版的 Virtua Racing，或 32X, Saturn 等平台移植版。

> **此為非官方專案**，與 SEGA 無關也無認可。Virtua Racing 是 SEGA 的商標。本專案不包含 ROM 或任何從 ROM 取出的資料。僅限非商業用途。

## 特色

透過高階模擬（High-Level Emulation，HLE），攔截 68000 CPU 傳送給 SVP 3D 處理晶片的繪圖指令，改由 GPU 繪製以提升畫面品質。

- **解析度**：將原本 256x192 解析度的畫面提升，並將 3D 頂點資訊由整數提升至浮點運算，修正多邊形抖動。
- **顏色**：提升色彩表現，移除網點狀混色。
- **FPS**：提升原本 15 FPS 表現，可選擇 30, 60 FPS 等模式，並且降低操作延遲提升反應速度。
- **寬螢幕**：可選擇原本 4:3 或者強化版 16:9。
- **可視距離**：遠景顯示距離提升約三倍。
- **內建選單**：可隨時調整強化項目。

<p align="center">
  <img src="docs/images/original.png" alt="原版" width="42%">
  <img src="docs/images/enhanced.png" alt="強化版" width="57%">
</p>
<p align="center">左：原版（4:3，256×224）。右：強化版（16:9，遠景加霧）。</p>

<p align="center">
  <img src="docs/images/menu.png" alt="遊戲內設定選單" width="75%">
</p>
<p align="center">遊戲內的設定選單（Esc）。</p>

<p align="center">
  <a href="https://youtu.be/zPLkgRMxFig"><img src="https://img.youtube.com/vi/zPLkgRMxFig/maxresdefault.jpg" alt="Virtua Racing MD Enhanced 展示影片" width="75%"></a>
</p>
<p align="center">YouTube 上的展示影片（點擊播放）。</p>

## ROM

本專案**不提供**任何 ROM。

目前支援的版本：

| 項目 | 值 |
|---|---|
| 版本 | Virtua Racing (USA)|
| SHA-1 | `ff969ae53120cc4e7cb1a8a7e47458f2eb8a2165` |
| CRC32 | `7e1a324a` |

## 啟動遊戲

到 Releases 下載已經編譯好的版本，再準備好自己的 ROM。
若對預先編譯的執行檔有疑慮，也可以自行從原始碼編譯。編譯步驟請參考 [technical.md](docs/technical.md)（英文）的「Building it yourself」。

### macOS

- 需搭載 Apple Silicon（M1 以後晶片）的 Mac。
- 需要先用 [Homebrew](https://brew.sh) 安裝 SDL2：`brew install sdl2`
- 在終端機裡，到 `vrr` 所在的資料夾執行：

  ```sh
  ./vrr "Virtua Racing (USA).md"
  ```

- 程式沒有數位簽章，macOS 可能會顯示「無法驗證開發者」而擋下來。到「系統設定 → 隱私權與安全性」，選擇「強制打開」即可。

### Windows

- 需要 64 位元的 Windows 10 以上。
- `vrr.exe` 和 `SDL2.dll` 放在同一個資料夾，把 ROM 檔拖到 `vrr.exe` 上就會啟動。
- 程式沒有數位簽章，第一次執行時可能會被擋下，可選擇「其他資訊」→「仍要執行」來強制開啟遊戲。

### Linux

已附 Makefile，編譯方式請參考 [technical.md](docs/technical.md)（英文）的「Building it yourself」。

設定檔（`vrr.ini`）和存檔（`vrr.gp0`）在 macOS 和 Linux 上存在執行時所在的資料夾，Windows 上存在 `vrr.exe` 旁邊。

## 操作

| 按鍵 | 功能 |
|---|---|
| 方向鍵 | 方向 |
| A / S / D | 手把的 A / B / C（比賽中 B 為油門） |
| Z / X / C | 六鍵手把的 X / Y / Z（比賽中切換視角） |
| F | Start |
| 1 | 切換檢視：HLE（預設）／原版／左右並排（原版 \| HLE） |
| 2 | 切換 HLE 的頂點資訊：浮點數（預設）／原版整數 |
| 3 | 切換 3D 解析度：視窗解析度（預設）／1 倍／2 倍／4 倍 |
| 4 | 切換反鋸齒：開啟（預設）／關閉 |
| 5 | 切換顏色：全彩（預設）／原版的 9-bit 調色盤和網點 |
| 6 | 切換 SVP 超頻（預設，修正 SVP 工作量大時導致的遊戲延遲）／原版 |
| 7 | 切換平順畫面：15 fps/30 fps/60 fps/顯示器更新率（預設） |
| 8 | 切換寬螢幕：16:9／原版的 4:3（預設） |
| 9 | 切換遠景：遠景加霧（預設）／遠景／原版 |
| O | 存檔：儲存目前狀態至 `vrr.gp0`（僅有一個存檔槽，會覆蓋先前的存檔） |
| P | 讀檔：從 `vrr.gp0` 讀回狀態 |
| Esc | 開啟選單：遊戲暫停，可用方向鍵和 Enter 調整設定、存檔、讀檔或離開，再按 Esc 關閉 |
| Alt+Enter | 切換全螢幕（無邊框，使用桌面的解析度） |

支援 Xbox、PlayStation、Switch Pro，以及其他 SDL 支援的手把。

主要按鍵依實體位置對應 Mega Drive 六鍵手把：左、下、右側按鍵分別對應 A、B、C，上方按鍵對應 Z。

| Xbox | PlayStation | Switch Pro | 對應 |
|---|---|---|---|
| 十字鍵、左類比搖桿 | 同左 | 同左 | 方向 |
| X（左）、左 trigger | □、L2 | Y、ZL | 手把的 A |
| A（下）、右 trigger | ✕、R2 | B、ZR | 手把的 B |
| B（右） | ○ | A | 手把的 C |
| LB、RB | L1、R1 | L、R | 六鍵手把的 X、Y |
| Y（上） | △ | X | 六鍵手把的 Z |
| Start | Options | + | Start |
| Back（或 Guide） | Share／Create（或 PS） | −（或 Home） | 打開選單 |


## 目錄

| 路徑 | 內容 |
|---|---|
| `src/` | 本專案的程式碼 |
| `tools/` | 研究工具：SSP1601 反組譯器、SVP 記憶體存取紀錄分析、VDP 圖層拆解 |
| `docs/` | 技術文件（英文） |
| `third_party/genplus-gx/` | Genesis Plus GX 的 `core/` 和 `sdl/` 前端。修改處均有標示 `VRR:` |

## 引用參考與感謝

- **notaz**（Grazvydas Ignotas）：SVP 模擬和 [SVP 文件](https://segaretro.org/SVP_documentation_(2014-09-23))。
- **Tasco Deluxe**：SVP 的先驅研究。
- **Devon**（Ralakimus）：[Sonic Retro 上的 Virtua Racing 研究](https://forums.sonicretro.org/threads/sega-virtua-processor-virtua-racing-research.38296/) 和 [svp-blender](https://github.com/Ralakimus/svp-blender)。
- **JaviTaiyou**（Javier de Silóniz Sandino）：[svpdev](https://github.com/jdesiloniz/svpdev) 的實機研究。
- **Eke-Eke**：[Genesis Plus GX](https://github.com/ekeeke/Genesis-Plus-GX)。

## 授權

本專案自己的程式碼和文件採用 [LICENSE](LICENSE) 的條款：僅限非商業用途，條件和 Genesis Plus GX 相同。`docs/images/` 的截圖是 SEGA 的遊戲畫面，不在授權範圍內。

## 第三方授權

本專案包含取自 [Genesis Plus GX](https://github.com/ekeeke/Genesis-Plus-GX)
commit `939ce4f045f981f89965f24780cef045cc5e52d7` 的程式碼，
位於 `third_party/genplus-gx/`。

Genesis Plus GX 採非商業授權。
完整條款請參閱 `third_party/genplus-gx/LICENSE.txt`。

其中的 SVP 模擬程式碼位於 `third_party/genplus-gx/core/cart_hw/svp/`，
由 notaz 撰寫，同樣限非商業用途。

散布或修改上述元件時，須遵守各自的授權條款。
