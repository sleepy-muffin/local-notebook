#include "support/i18n.hpp"

#include <iterator>
#include <unordered_map>

namespace lnb::i18n {

namespace {

// One translation entry: the English source text (the lookup key) and its
// Chinese and Japanese renderings. Entries are grouped by the window or page
// they belong to; within a group they follow the source order, which keeps the
// table easy to diff against the code.
struct Entry {
  std::string_view en;
  const char* zh; // UTF-8
  const char* ja; // UTF-8
};

constexpr Entry kTranslations[] = {
    // General.
    {"Close", "关闭", "閉じる"},
    {"Save", "保存", "保存"},
    {"Cancel", "取消", "キャンセル"},
    {"Create", "创建", "作成"},
    {"Edit", "编辑", "編集"},
    {"Copy", "复制", "コピー"},
    {"Select all", "全选", "すべて選択"},
    {"Open", "打开", "開く"},
    {"Run", "运行", "実行"},
    {"Settings", "设置", "設定"},
    {"Configuration", "配置", "設定"},
    {"Metrics", "度量", "メトリクス"},
    {"Assistant", "助手", "アシスタント"},
    {"Account", "账户", "アカウント"},
    {"About", "关于", "このアプリについて"},
    {"Language", "语言", "言語"},
    {"Language changed", "语言已更改", "言語を変更しました"},
    {"The interface language will change after Local Notebook restarts. Restart now?", "界面语言将在 Local Notebook 重启后更改。现在重启吗？", "インターフェースの言語は Local Notebook の再起動後に変更されます。今すぐ再起動しますか？"},
    // Error messages shown in the transcript.
    {"Unknown summarisation error", "未知的总结错误", "不明な要約エラー"},
    {"The request failed for an unknown reason.", "请求因未知原因失败。", "不明な理由でリクエストが失敗しました。"},
    // Configuration page.
    {"Appearance", "外观", "外観"},
    {"Theme", "主题", "テーマ"},
    {"Interface font", "界面字体", "インターフェースフォント"},
    {"Interface size", "界面字号", "インターフェースのサイズ"},
    {"Chat font", "聊天字体", "チャットフォント"},
    {"Chat size", "聊天字号", "チャットのサイズ"},
    {"Monospace font", "等宽字体", "等幅フォント"},
    {"Monospace size", "等宽字号", "等幅フォントのサイズ"},
    {"Inline code size", "内联代码字号", "インラインコードのサイズ"},
    {"Inline code size as a fraction of the surrounding text.", "内联代码字号相对于周围文字的比例。", "インラインコードのサイズを周囲のテキストに対する比率で指定します。"},
    {"Composer", "输入框", "入力欄"},
    {"Send with Ctrl+Enter", "使用 Ctrl+Enter 发送", "Ctrl+Enter で送信"},
    {"When on, Ctrl+Enter sends and Enter inserts a new line.", "开启时，Ctrl+Enter 发送，Enter 换行。", "オンのとき、Ctrl+Enter で送信し、Enter で改行します。"},
    {"Transcript", "对话记录", "トランスクリプト"},
    {"Rendered messages", "渲染的消息数", "表示するメッセージ数"},
    {"History page size", "历史分页大小", "履歴のページサイズ"},
    {"Background", "背景", "背景"},
    {"Image", "图片", "画像"},
    {"Path to a background image (optional)", "背景图片路径（可选）", "背景画像のパス（任意）"},
    {"Tint the image", "为图片着色", "画像に着色する"},
    {"Restore to defaults", "恢复默认值", "デフォルトに戻す"},
    {"Reset this page's fields to their shipped defaults. Nothing is saved until you press Save.", "将此页面的字段重置为出厂默认值。在你按下“保存”之前不会保存任何内容。", "このページの項目を出荷時のデフォルトに戻します。「保存」を押すまで何も保存されません。"},
    {"Application-wide options stored in data/settings.json.", "存储在 data/settings.json 中的应用级选项。", "data/settings.json に保存されるアプリ全体のオプション。"},
    // Metrics page.
    {"Layout", "布局", "レイアウト"},
    {"Sidebar width", "侧边栏宽度", "サイドバー幅"},
    {"Page margin", "页面边距", "ページ余白"},
    {"Message spacing", "消息间距", "メッセージ間隔"},
    {"Composer height", "输入框高度", "入力欄の高さ"},
    {"Expanded height", "展开高度", "展開時の高さ"},
    {"Session item height", "会话项高度", "セッション項目の高さ"},
    {"Title font size", "标题字号", "タイトル文字サイズ"},
    {"Flow max width", "正文最大宽度", "本文の最大幅"},
    {"Bubble radius", "气泡圆角", "吹き出しの角丸"},
    {"Reading rhythm", "阅读节奏", "リーディングリズム"},
    {"Line heights are percentages (100 is single spacing).", "行高为百分比（100 为单倍行距）。", "行の高さはパーセントです（100 が単一倍）。"},
    {"Assistant line height", "助手行高", "アシスタントの行の高さ"},
    {"User line height", "用户行高", "ユーザーの行の高さ"},
    {"Paragraph spacing", "段间距", "段落間隔"},
    {"List spacing", "列表间距", "リスト間隔"},
    {"Reset these metrics to the current theme's own defaults. Nothing is saved until you press Save.", "将这些度量重置为当前主题自身的默认值。在你按下“保存”之前不会保存任何内容。", "これらのメトリクスを現在のテーマ自身のデフォルトに戻します。「保存」を押すまで何も保存されません。"},
    {"Sizing and spacing of the interface.", "界面的尺寸与间距。", "インターフェースのサイズと間隔。"},
    // Assistant page.
    {"Assistant memory", "助手记忆", "アシスタントメモリ"},
    {"Summarize now", "立即总结", "今すぐ要約"},
    {"Fold the recent messages of every session into the memory below, ignoring the automatic trigger. Idempotent: pressing it again with no new messages changes nothing.", "忽略自动触发，将会话中所有最近的消息折叠到下面的记忆中。幂等：在没有新消息时再次按下不会有任何改变。", "自動トリガーを無視して、すべてのセッションの最近のメッセージを下のメモリに畳み込みます。冪等：新しいメッセージがない状態で再度押しても何も変わりません。"},
    {"Automatically maintained summary shared across every session. Edit to correct it, or clear it to rebuild it from the transcript.", "自动维护的、在所有会话间共享的摘要。可以编辑以修正，或清空以从对话记录重新构建。", "すべてのセッションで共有される自動維持の要約です。編集して修正するか、消去してトランスクリプトから再構築してください。"},
    {"<b>How assistant memory works</b><br>This memory is shared across every session of the assistant. It is folded from the recent messages of all sessions (bounded by <i>Trigger tokens</i>) together with each session's own summary (bounded by <i>Session pool</i>). Older sessions may therefore be forgotten.<br><br><b>Summarize now</b> ignores the automatic trigger and folds on demand. It is idempotent: running it again with no new messages reads the same set and produces the same memory. Custom instructions are kept separate and are never touched.", "<b>助手记忆的工作原理</b><br>此记忆在该助手的每个会话之间共享。它由所有会话的最近消息（受 <i>触发 token 数</i> 限制）以及每个会话自身的摘要（受 <i>会话池</i> 限制）折叠而成。因此较早的会话可能会被遗忘。<br><br><b>立即总结</b> 忽略自动触发并按需折叠。它是幂等的：在没有新消息时再次运行会读取同一组内容并产生相同的记忆。自定义指令单独保存，永远不会被触碰。", "<b>アシスタントメモリの仕組み</b><br>このメモリはアシスタントのすべてのセッションで共有されます。すべてのセッションの最近のメッセージ（<i>トリガートークン数</i>で上限）と、各セッション自身の要約（<i>セッションプール</i>で上限）から畳み込まれます。そのため、古いセッションは忘れられる可能性があります。<br><br><b>今すぐ要約</b> は自動トリガーを無視してオンデマンドで折り畳みます。冪等であり、新しいメッセージがない状態で再度実行しても同じ集合を読み取り、同じメモリを生成します。カスタム指示は別に保持され、決して変更されません。"},
    {"Summarizing...", "正在总结...", "要約中..."},
    {"Summarization Failed", "总结失败", "要約に失敗しました"},
    {"Nothing to Summarize", "无可总结内容", "要約するものがありません"},
    {"There is no new conversation to fold into memory yet.", "还没有可折叠到记忆中的新对话。", "メモリに畳み込む新しい会話はまだありません。"},
    {"The assistant open in this window. It is fixed for this run, but its configuration and memory can be edited here.", "此窗口中打开的助手。它在本次运行期间固定不变，但其配置和记忆可以在此编辑。", "このウィンドウで開いているアシスタント。この実行中は固定ですが、構成とメモリはここで編集できます。"},
    // Account page.
    {"Current session", "当前会话", "現在のセッション"},
    {"Startup", "启动", "起動"},
    {"Open at startup", "启动时打开", "起動時に開く"},
    {"Open this assistant automatically at startup. When off, the assistant picker is shown every time.", "启动时自动打开此助手。关闭时，每次都会显示助手选择器。", "起動時にこのアシスタントを自動的に開きます。オフのときは毎回アシスタント選択画面が表示されます。"},
    {"No session selected", "未选择会话", "セッションが選択されていません"},
    {"Session #%1", "会话 #%1", "セッション #%1"},
    {"The session you are in, and how the app starts.", "你所在的会话，以及应用的启动方式。", "現在のセッションと、アプリの起動方法。"},
    // About page.
    {"A local, assistant-backed notebook.", "一个本地、由助手驱动的笔记本。", "ローカルでアシスタントを備えたノートブック。"},
    {"Version %1", "版本 %1", "バージョン %1"},
    {"Built with Qt %1", "使用 Qt %1 构建", "Qt %1 でビルド"},
    {"License", "许可", "ライセンス"},
    {"Distributed under the terms in LICENSE.txt.", "依据 LICENSE.txt 中的条款分发。", "LICENSE.txt の条項に基づいて配布されています。"},
    {"Program information.", "程序信息。", "プログラム情報。"},
    // Main window.
    {"Toggle sidebar", "切换侧边栏", "サイドバーの表示切替"},
    {"New Session", "新建会话", "新しいセッション"},
    {"PINNED", "已固定", "ピン留め"},
    {"SESSIONS", "会话", "セッション"},
    {"Select a session, or create one to start.", "选择一个会话，或新建一个以开始。", "セッションを選択するか、新規作成して開始してください。"},
    {"No tasks running", "没有正在运行的任务", "実行中のタスクはありません"},
    {"1 task running", "1 个任务正在运行", "1 件のタスクを実行中"},
    {"%1 tasks running", "%1 个任务正在运行", "%1 件のタスクを実行中"},
    {"Running", "运行中", "実行中"},
    {"Completed", "已完成", "完了"},
    {"Failed", "失败", "失敗"},
    {"Cancelled", "已取消", "キャンセル済み"},
    {"Delete Session", "删除会话", "セッションの削除"},
    {"Are you sure you want to delete this session? This cannot be undone.", "确定要删除此会话吗？此操作无法撤销。", "このセッションを削除してもよろしいですか？この操作は取り消せません。"},
    {"summarisation trigger: ", "摘要触发阈值：", "要約トリガー: "},
    {"%1 / %2 conversation tokens (system prompt excluded; the collapse fires at the limit)", "%1 / %2 会话 token（不含系统提示词；达到上限时触发压缩）", "%1 / %2 会話トークン（システムプロンプトを除く。上限に達すると圧縮が実行されます）"},
    // Conversation view.
    {"%1 tok", "%1 词元", "%1 トークン"},
    {"%1k tok", "%1k 词元", "%1k トークン"},
    {"Type a message...  (Ctrl+Enter to send, Enter for a new line)", "输入消息...  （Ctrl+Enter 发送，Enter 换行）", "メッセージを入力...  （Ctrl+Enter で送信、Enter で改行）"},
    {"Type a message...  (Enter to send, Shift+Enter for a new line)", "输入消息...  （Enter 发送，Shift+Enter 换行）", "メッセージを入力...  （Enter で送信、Shift+Enter で改行）"},
    {"Copy message", "复制消息", "メッセージをコピー"},
    {"Edit message", "编辑消息", "メッセージを編集"},
    {"Delete message", "删除消息", "メッセージを削除"},
    {"Message:", "消息：", "メッセージ："},
    {"Summarize the conversation into memory", "将对话总结到记忆中", "会話をメモリに要約する"},
    {"Send", "发送", "送信"},
    {"Shrink the composer", "缩小输入框", "入力欄を縮小"},
    {"Expand the composer", "展开输入框", "入力欄を展開"},
    // Assistant picker.
    {"Assistants", "助手", "アシスタント"},
    {"Choose an assistant to open, or create a new one.", "选择要打开的助手，或新建一个。", "開くアシスタントを選択するか、新規作成します。"},
    {"New assistant", "新建助手", "新規アシスタント"},
    {"Open automatically at startup", "启动时自动打开", "起動時に自動的に開く"},
    {"Untitled assistant", "未命名助手", "無題のアシスタント"},
    {"No assistant selected", "未选择助手", "アシスタントが選択されていません"},
    {"There are no assistants yet. Create one to get started.", "还没有助手。新建一个即可开始。", "アシスタントがまだありません。作成して始めましょう。"},
    {"Select an assistant on the left to see it here.", "在左侧选择一个助手以在此查看。", "左側のアシスタントを選択するとここに表示されます。"},
    {"Edit assistant", "编辑助手", "アシスタントを編集"},
    // Session editor.
    {"Edit Session", "编辑会话", "セッションを編集"},
    {"Session", "会话", "セッション"},
    {"Session title", "会话标题", "セッションのタイトル"},
    {"Directories the assistant may access, one per line (optional)", "助手可访问的目录，每行一个（可选）", "アシスタントがアクセスできるディレクトリ（1行に1つ、任意）"},
    {"Title", "标题", "タイトル"},
    {"Shared directories", "共享目录", "共有ディレクトリ"},
    {"Tool permissions", "工具权限", "ツールの権限"},
    {"Enable for this session", "为此会话启用", "このセッションで有効にする"},
    {"When off, no tools are offered to the model for this session. The switches below keep their values.", "关闭时，不会向该会话的模型提供任何工具。下方的开关会保留其取值。", "オフのとき、このセッションのモデルにはツールが提供されません。下のスイッチは値を保持します。"},
    {"Read files", "读取文件", "ファイルを読み取る"},
    {"Allow listing, searching and reading files in the shared directories.", "允许在共享目录中列出、搜索和读取文件。", "共有ディレクトリ内のファイルの一覧表示、検索、読み取りを許可します。"},
    {"Write files", "写入文件", "ファイルに書き込む"},
    {"Allow creating, overwriting, editing and deleting files in the shared directories.", "允许在共享目录中创建、覆盖、编辑和删除文件。", "共有ディレクトリ内のファイルの作成、上書き、編集、削除を許可します。"},
    {"Search online", "在线搜索", "オンライン検索"},
    {"Allow web searches and fetching web pages.", "允许搜索网络并获取网页。", "ウェブ検索とページの取得を許可します。"},
    {"Tools", "工具", "ツール"},
    {"Read", "读取", "読み取り"},
    {"Write", "写入", "書き込み"},
    {"Online", "在线", "オンライン"},
    {"Custom instructions", "自定义指令", "カスタム指示"},
    {"Kept separate from the generated memory; never overwritten by it.", "与生成的记忆分开保存；永远不会被其覆盖。", "生成されたメモリとは別に保持され、上書きされることはありません。"},
    {"Conversation memory", "对话记忆", "会話メモリ"},
    {"Fold the conversation into the memory below, ignoring the automatic trigger. Idempotent: pressing it again with no new messages changes nothing.", "忽略自动触发，将对话折叠到下面的记忆中。幂等：在没有新消息时再次按下不会有任何改变。", "自動トリガーを無視して、会話を下のメモリに畳み込みます。冪等：新しいメッセージがない状態で再度押しても何も変わりません。"},
    {"Automatically maintained summary of older turns. Edit to correct it, or clear it to rebuild it from the whole transcript.", "自动维护的较早轮次摘要。可编辑以修正，或清空以从完整对话记录重新构建。", "古いやり取りの自動維持される要約です。編集して修正するか、消去してトランスクリプト全体から再構築してください。"},
    {"Session fold frontier: the highest message id already folded into the memory above. Used only to decide when the automatic trigger fires.", "会话折叠边界：已折叠到上方记忆中的最大消息 ID。仅用于判断自动触发何时发生。", "セッション折り畳み境界：上のメモリに既に畳み込まれた最大メッセージ ID。自動トリガーの発火時期を判断するためにのみ使用されます。"},
    {"Assistant fold frontier for this session: the highest message id already folded into the shared assistant memory.", "此会话的助手折叠边界：已折叠到共享助手记忆中的最大消息 ID。", "このセッションのアシスタント折り畳み境界：共有アシスタントメモリに既に畳み込まれた最大メッセージ ID。"},
    {"Memory watermark", "记忆水位", "メモリウォーターマーク"},
    {"Assistant watermark", "助手水位", "アシスタントウォーターマーク"},
    {"Budgets", "预算", "予算"},
    {"Apply a preset...", "应用预设...", "プリセットを適用..."},
    {"Preset", "预设", "プリセット"},
    {"Fill the budgets below with a preset. Nothing is saved until you press Save.", "使用预设填充下面的预算。在你按下“保存”之前不会保存任何内容。", "プリセットで下のバジェットを入力します。「保存」を押すまで何も保存されません。"},
    {"Summary trigger", "总结触发阈值", "要約トリガー"},
    {"Retained tokens", "保留 token 数", "保持トークン数"},
    {"Memory budget", "记忆预算", "メモリバジェット"},
    {"Reasoning headroom", "推理余量", "推論ヘッドルーム"},
    {"Response budget", "回复预算", "応答バジェット"},
    {"Tool output cap", "工具输出上限", "ツール出力上限"},
    {"Max tool rounds", "最大工具轮数", "ツールの最大ラウンド数"},
    {"Max tool calls", "最大工具调用数", "ツールの最大呼び出し数"},
    {"Invalid Budget", "预算无效", "無効なバジェット"},
    {"The response budget must not be greater than the summarization trigger. A reply could otherwise never complete before the conversation is collapsed again.", "回复预算不得大于总结触发阈值。否则回复可能永远无法在对话再次折叠之前完成。", "応答バジェットは要約トリガー以下でなければなりません。そうでないと、会話が再び折り畳まれる前に返信が完了できない可能性があります。"},
    {"The retained-token level must be smaller than the summarization trigger. Otherwise the collapse would be triggered again immediately after it runs.", "保留 token 数必须小于总结触发阈值。否则折叠会在运行后立即再次触发。", "保持トークン数は要約トリガーより小さくなければなりません。そうでないと、折り畳みが実行直後に再びトリガーされます。"},
    {"<b>How summarisation works</b><br>Conversation memory is a rolling summary of older turns. A request carries the system prompt plus the transcript; when that reaches <i>Summary trigger</i>, the older messages are folded into the memory, collapsing the transcript to the newest <i>Retained tokens</i>. Keep the retained level well below the trigger, or the collapse would run again on the next turn. The message that crosses the boundary is kept whole.<br><br>The watermarks above are <b>bookkeeping only</b>: they count how much is new so the trigger can fire. They never decide what is sent or what is read — the summariser always reads by recency, so pressing <b>Summarize now</b> twice with no new messages reads the same set and produces the same memory (idempotent). Custom instructions are kept separate and are never touched.<br><br>The <b>assistant memory</b> is shared across every session: it is folded from the recent messages of all sessions (bounded by the assistant's own trigger) plus each session's summary (bounded by the session-pool budget). Older sessions may therefore be forgotten.<br><br>Clearing the memory and saving resets its watermark to 0, re-admitting the whole transcript.", "<b>总结的工作原理</b><br>对话记忆是较早轮次的滚动摘要。每次请求都会携带系统提示词以及对话记录；当它达到 <i>总结触发阈值</i> 时，较早的消息会被折叠进记忆中，将对话记录压缩到最新的 <i>保留 token 数</i>。请让保留水平远低于触发阈值，否则折叠会在下一轮再次运行。跨越边界的消息会完整保留。<br><br>上方的水位<b>仅用于记账</b>：它们统计有多少新内容，以便触发阈值能够触发。它们绝不决定发送或读取什么——总结器始终按最近程度读取，因此在没有新消息时按两次 <b>立即总结</b> 会读取同一组内容并产生相同的记忆（幂等）。自定义指令单独保存，永远不会被触碰。<br><br><b>助手记忆</b>在所有会话之间共享：它由所有会话的最近消息（受助手自身触发阈值限制）加上每个会话的摘要（受会话池预算限制）折叠而成。因此较早的会话可能会被遗忘。<br><br>清空记忆并保存会将其水位重置为 0，从而重新纳入整个对话记录。", "<b>要約の仕組み</b><br>会話メモリは古いやり取りのローリング要約です。リクエストにはシステムプロンプトとトランスクリプトが含まれ、それが <i>要約トリガー</i> に達すると、古いメッセージがメモリに畳み込まれ、トランスクリプトは最新の <i>保持トークン数</i> まで圧縮されます。保持レベルはトリガーを十分に下回るようにしてください。そうでないと、次のターンで再び折り畳みが実行されます。境界を越えるメッセージはそのまま保持されます。<br><br>上のウォーターマークは<b>記帳専用</b>です。どれだけ新しいか数えてトリガーを発火させるためだけに使用されます。何を送信し何を読み取るかを決定することはありません。要約器は常に新しい順に読み取るため、新しいメッセージがない状態で <b>今すぐ要約</b> を2回押しても同じ集合を読み取り、同じメモリを生成します（冪等）。カスタム指示は別に保持され、決して変更されません。<br><br><b>アシスタントメモリ</b>はすべてのセッションで共有されます。これはすべてのセッションの最近のメッセージ（アシスタント自身のトリガーで上限）と各セッションの要約（セッションプールのバジェットで上限）から折り畳まれます。そのため、古いセッションは忘れられる可能性があります。<br><br>メモリを消去して保存すると、ウォーターマークが 0 にリセットされ、トランスクリプト全体が再び取り込まれます。"},
    // Presets.
    {"Instant", "极轻", "最軽量"},
    {"Very Light", "极轻", "最軽量"},
    {"Light", "轻量", "軽量"},
    {"Balanced", "均衡", "バランス"},
    {"Heavy", "重度", "ヘビー"},
    {"Very Heavy", "极重", "超ヘビー"},
    {"Ultra", "极致", "ウルトラ"},
    // Assistant form.
    {"Assistant name", "助手名称", "アシスタント名"},
    {"Model id, e.g. gpt-4o", "模型 ID，例如 gpt-4o", "モデル ID（例: gpt-4o）"},
    {"Name", "名称", "名前"},
    {"Model", "模型", "モデル"},
    {"Connection", "连接", "接続"},
    {"https://api.example.com/v1/chat/completions", "https://api.example.com/v1/chat/completions", "https://api.example.com/v1/chat/completions"},
    {"Authentication token", "身份验证令牌", "認証トークン"},
    {"Optional", "可选", "任意"},
    {"API URL", "API URL", "API URL"},
    {"Token", "令牌", "トークン"},
    {"Proxy", "代理", "プロキシ"},
    {"Turn timeout", "回合超时", "ターンタイムアウト"},
    {"Summarisation timeout", "总结超时", "要約タイムアウト"},
    {"Seconds a request may wait for the next response byte before it is abandoned. Chat turns use the turn timeout; session and assistant summarisation use the summarisation timeout. Connecting and uploading keep a fixed 60-second window.", "请求在放弃前等待下一个响应字节的秒数。聊天回合使用回合超时；会话和助手总结使用总结超时。连接和上传保持固定的 60 秒窗口。", "リクエストが破棄されるまでに次の応答バイトを待つ秒数。チャットのターンはターンタイムアウトを、セッションおよびアシスタントの要約は要約タイムアウトを使用します。接続とアップロードは固定の 60 秒ウィンドウのままです。"},
    {"Maximum silence, in seconds, while a chat turn streams its reply.", "聊天回合流式回复时允许的最长静默秒数。", "チャットのターンが応答をストリーミングする際に許容される最大の無音秒数。"},
    {"Maximum silence, in seconds, while a session or assistant summarisation runs. Reasoning models often need longer.", "会话或助手总结运行期间允许的最长静默秒数。推理模型通常需要更长时间。", "セッションまたはアシスタントの要約の実行中に許容される最大の無音秒数。推論モデルではより長く必要になることがよくあります。"},
    {"System prompt for this assistant", "此助手的系统提示词", "このアシスタントのシステムプロンプト"},
    {"Summarisation", "摘要", "要約"},
    {"Target size of the generated assistant memory.", "生成的助手记忆的目标大小。", "生成されるアシスタントメモリの目標サイズ。"},
    {"Extra completion budget for a reasoning model's hidden thinking, so the visible memory is not starved.", "为推理模型的隐藏思考预留的额外补全预算，以免可见记忆被挤占。", "推論モデルの隠れた思考のための追加の補完バジェット。可視メモリが逼迫しないようにします。"},
    {"New tokens across all sessions that trigger the summariser; also the cap on the recent messages gathered.", "所有会话中触发摘要器的新 token 总数；同时也是所收集最近消息的上限。", "要約を起動する全セッションの新規トークン数。収集される最近のメッセージの上限でもあります。"},
    {"Token cap on the per-session summaries included in the assistant fold.", "包含在助手汇总中的每个会话摘要的 token 上限。", "アシスタントの統合に含まれるセッションごとの要約のトークン上限。"},
    {"Trigger tokens", "触发 token 数", "トリガートークン数"},
    {"Session pool", "会话池", "セッションプール"},
    // Tasks window.
    {"%1 s", "%1 秒", "%1 秒"},
    {"This task produced no log.", "此任务未生成日志。", "このタスクはログを生成しませんでした。"},
    {"Tasks", "任务", "タスク"},
    {"Running and recently finished generation tasks. Select one to read its log.", "正在运行和最近完成的生成任务。选择一项以查看其日志。", "実行中および最近完了した生成タスク。項目を選択するとログを確認できます。"},
    {"No tasks yet.", "暂无任务。", "タスクはまだありません。"},
    {"Session #%1   ·   %2   ·   Round %3   ·   %4", "会话 #%1   ·   %2   ·   第 %3 轮   ·   %4", "セッション #%1   ·   %2   ·   ラウンド %3   ·   %4"},
    // Summary review dialog.
    {"Review summarization", "审查摘要", "要約の確認"},
    {"The following request is about to be sent to the model to update the %1. Review it and choose whether to run the summarisation.", "以下请求即将发送给模型以更新 %1。请审阅，并选择是否运行摘要。", "以下のリクエストが %1 を更新するためにモデルへ送信されようとしています。確認して、要約を実行するか選択してください。"},
    {"This is the first of several requests: the remaining messages are folded in further batches after this one.", "这是多个请求中的第一个：其余消息将在本次之后分批进一步折叠。", "これは複数のリクエストの最初のものです。残りのメッセージはこの後さらにバッチで畳み込まれます。"},
    {"Summarization Skipped", "已跳过摘要", "要約をスキップしました"},
    {"The automatic summarisation was skipped. Some messages may not be folded into memory; use Summarize now before sending new messages if you want them converged.", "自动摘要已跳过。部分消息可能尚未折叠到记忆中；若希望它们合并，请在发送新消息前使用“立即总结”。", "自動要約がスキップされました。一部のメッセージがメモリに畳み込まれていない可能性があります。統合したい場合は、新しいメッセージを送信する前に「今すぐ要約」を使用してください。"},
    {"Model: %1\n", "模型：%1\n", "モデル: %1\n"},
    {"Completion cap: %1 tokens\n\n", "补全上限：%1 个 token\n\n", "補完上限: %1 トークン\n\n"},
    // Tool status.
    {"Reading files...", "正在读取文件...", "ファイルを読み込み中..."},
    {"Searching code...", "正在搜索代码...", "コードを検索中..."},
    {"Listing files...", "正在列出文件...", "ファイルを一覧表示中..."},
    {"Finding files...", "正在查找文件...", "ファイルを検索中..."},
    {"Writing file...", "正在写入文件...", "ファイルに書き込み中..."},
    {"Editing file...", "正在编辑文件...", "ファイルを編集中..."},
    {"Deleting file...", "正在删除文件...", "ファイルを削除中..."},
    {"Fetching page...", "正在获取页面...", "ページを取得中..."},
    {"Searching the web...", "正在搜索网络...", "ウェブを検索中..."},
    {"Using tools...", "正在使用工具...", "ツールを使用中..."},
};

const std::unordered_map<std::string_view, Entry>& table() {
  static const std::unordered_map<std::string_view, Entry> map = [] {
    std::unordered_map<std::string_view, Entry> built;
    built.reserve(std::size(kTranslations));
    for (const Entry& entry : kTranslations) {
      built.emplace(entry.en, entry);
    }
    return built;
  }();
  return map;
}

Language g_language = Language::English;

QString from_utf8(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<int>(text.size()));
}

} // namespace

Language language_from_code(std::string_view code) {
  if (code == "zh") {
    return Language::Chinese;
  }
  if (code == "ja") {
    return Language::Japanese;
  }
  return Language::English;
}

std::string_view code_from_language(Language language) {
  switch (language) {
  case Language::Chinese:
    return "zh";
  case Language::Japanese:
    return "ja";
  case Language::English:
    break;
  }
  return "en";
}

QString language_name(Language language) {
  switch (language) {
  case Language::Chinese:
    return QString::fromUtf8("中文");
  case Language::Japanese:
    return QString::fromUtf8("日本語");
  case Language::English:
    break;
  }
  return QStringLiteral("English");
}

void set_language(Language language) { g_language = language; }

Language language() { return g_language; }

QString translate(std::string_view source) {
  if (g_language == Language::English) {
    return from_utf8(source);
  }

  const auto found = table().find(source);
  if (found == table().end()) {
    return from_utf8(source);
  }

  const char* value =
      g_language == Language::Chinese ? found->second.zh : found->second.ja;
  return value != nullptr ? QString::fromUtf8(value) : from_utf8(source);
}

} // namespace lnb::i18n
