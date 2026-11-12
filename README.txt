GTO 翻後解算器 — 使用說明

【Windows 最快用法】
  雙擊 start.bat → 瀏覽器會開啟「六人桌」介面：
    1. 在牌桌上點兩個座位（先行動者自動當開池者），選 SRP / 3-bet / 4-bet 底池
    2. 點選 3–5 張牌面（或按「隨機翻牌」），進階設定可調籌碼、下注尺度、迭代次數
    3. 按「開始解算」，完成後下方直接顯示結果（牌桌上會標出目前行動者與每條街的動作）
  使用期間請保持 start.bat 開出的黑色視窗開著；關掉視窗即停止。
  介面上的設定會寫回 config.txt，所以也可以照下面的方式用指令列執行。
  solver.exe 已編譯好（需支援 AVX2 的 CPU，約 2013 年後的 Intel/AMD 皆可）；
  若換電腦無法執行，執行 build.bat 重新編譯即可。

【編譯】（每台電腦只需做一次，把整個資料夾複製過去即可）
  macOS / Linux：在終端機執行  ./build.sh        （需要 Xcode Command Line Tools 或 g++/clang++）
  Windows：      雙擊或執行    build.bat         （需要 Visual Studio 的 C++ 工具，或 MinGW-w64 的 g++）

【執行】
  macOS / Linux：./solver config.txt
  Windows：      solver.exe config.txt

【看結果】
  用瀏覽器開啟 viewer.html（需與 result.js 放在同一個資料夾）。

【設定】
  所有可調項目都在 config.txt，內有註解。
  程式開始時會印出預估記憶體用量；若超過電腦記憶體，請減少下注尺度、縮小範圍或降低 raise_cap。
  isomorphism（花色同構）只在雙方範圍對該兩個花色完全對稱時才會生效；
  若範圍裡指定了單一花色組合（例如 AhKh），相關花色就不會合併，結果仍然正確，只是較慢。

【六人桌對決】
  在 config.txt 設定 matchup（例如 CO vs BTN）與 pot_type（srp / 3bp / 4bp），
  程式會從 ranges_6max.txt 帶入雙方範圍，並算出底池、剩餘籌碼與誰先行動。
  ranges_6max.txt 內是近似的翻前範圍，可換成你自己的。
  注意：翻後解算固定為兩人對決；三人以上同時在底池的多人底池不支援。
