<?php
// Ratrix Bot v2 - Web Dashboard
// Single-file backend & frontend.

session_start();

$db_file = __DIR__ . '/bot.sqlite';
$results_dir = __DIR__ . '/results';
$is_linux = (PHP_OS_FAMILY === 'Linux');
$tmux_session = 'ratrix_train';
$log_file = __DIR__ . '/train.log';
$live_session = 'ratrix_live';
$live_log = __DIR__ . '/live.log';
$live_csv = __DIR__ . '/live_trades.csv';

// ============================================================================
// 1. Database Initialization
// ============================================================================
try {
    $db = new PDO('sqlite:' . $db_file);
    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    
    // Create tables if they don't exist
    $db->exec("CREATE TABLE IF NOT EXISTS runs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        run_id TEXT,
        symbol TEXT,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
        top_strategy TEXT,
        top_win_rate REAL,
        top_pnl REAL,
        rl_pnl REAL,
        is_synced INTEGER DEFAULT 1,
        UNIQUE(run_id, symbol)
    )");
    
} catch(PDOException $e) {
    die(json_encode(["error" => "Database connection failed: " . $e->getMessage()]));
}

// Helper: Sync results folder into SQLite
function syncResultsToDB($db, $results_dir) {
    if (!is_dir($results_dir)) return;
    
    $run_dirs = glob($results_dir . '/run_*', GLOB_ONLYDIR);
    $existing_runs_stmt = $db->query("SELECT run_id, symbol FROM runs");
    $existing = [];
    while ($row = $existing_runs_stmt->fetch(PDO::FETCH_ASSOC)) {
        $existing[$row['run_id'] . '_' . $row['symbol']] = true;
    }
    
    foreach ($run_dirs as $run_dir) {
        $run_id = basename($run_dir);
        
        // Find symbols inside this run
        $symbol_dirs = glob($run_dir . '/*', GLOB_ONLYDIR);
        foreach ($symbol_dirs as $sym_dir) {
            $symbol = basename($sym_dir);
            
            if (isset($existing[$run_id . '_' . $symbol])) continue; // Already synced
            
            $summary_path = $sym_dir . '/summary.json';
            
            if (file_exists($summary_path)) {
                $json = json_decode(file_get_contents($summary_path), true);
                if (!$json) continue;
                
                $top_strat = isset($json['strategies'][0]) ? $json['strategies'][0]['strategyName'] : 'N/A';
                $top_wr = isset($json['strategies'][0]) ? $json['strategies'][0]['winRatePct'] : 0;
                $top_pnl = isset($json['strategies'][0]) ? $json['strategies'][0]['netPnl'] : 0;
                $rl_pnl = isset($json['rl_result']['netPnl']) ? $json['rl_result']['netPnl'] : 0;
                
                $stmt = $db->prepare("INSERT OR IGNORE INTO runs (run_id, symbol, top_strategy, top_win_rate, top_pnl, rl_pnl) VALUES (?, ?, ?, ?, ?, ?)");
                $stmt->execute([$run_id, $symbol, $top_strat, $top_wr, $top_pnl, $rl_pnl]);
            }
        }
    }
}

// ============================================================================
// 2. API Endpoints
// ============================================================================
if (isset($_GET['action'])) {
    header('Content-Type: application/json');
    $action = $_GET['action'];
    
    if ($action === 'status') {
        $status = 'stopped';
        if ($is_linux) {
            $check = shell_exec("tmux has-session -t $tmux_session 2>&1");
            if (strpos($check, 'no server') === false && strpos($check, 'can\'t find session') === false) {
                $status = 'running';
            }
        } else {
            // Windows check (naive)
            $check = shell_exec("tasklist | findstr derivbot");
            if (strpos($check, 'derivbot') !== false) {
                $status = 'running';
            }
        }
        echo json_encode(["status" => $status]);
        exit;
    }
    
    if ($action === 'start_train') {
        if ($is_linux) {
            // Check if already running
            shell_exec("tmux has-session -t $tmux_session 2>&1");
            // Build command
            $symbol = escapeshellarg($_POST['symbol'] ?? 'jump10');
            $cmd = "./derivbot --train --symbols $symbol";
            
            if (!empty($_POST['seed_run'])) {
                $cmd .= " --seed-run " . escapeshellarg($_POST['seed_run']);
                $cmd .= " --seed-rank " . (int)($_POST['seed_rank'] ?? 1);
            }
            if (!empty($_POST['tp'])) $cmd .= " --tp " . (float)$_POST['tp'];
            if (!empty($_POST['sl'])) $cmd .= " --sl " . (float)$_POST['sl'];
            if (!empty($_POST['lot'])) $cmd .= " --lot " . (float)$_POST['lot'];
            if (!empty($_POST['spread'])) $cmd .= " --spread " . (float)$_POST['spread'];
            if (isset($_POST['autoadjust']) && $_POST['autoadjust'] == 'true') {
                $cmd .= " --autoadjust";
            }
            
            // Advanced parameters
            if (!empty($_POST['history'])) $cmd .= " --train-history " . (int)$_POST['history'];
            if (!empty($_POST['strategies'])) $cmd .= " --strategies " . escapeshellarg($_POST['strategies']);
            if (!empty($_POST['durations'])) $cmd .= " --durations " . escapeshellarg($_POST['durations']);
            if (!empty($_POST['candles'])) $cmd .= " --candles " . escapeshellarg($_POST['candles']);
            if (isset($_POST['no_rl']) && $_POST['no_rl'] == 'true') {
                $cmd .= " --no-rl";
            }
            
            // clear old log
            if(file_exists($log_file)) unlink($log_file);
            
            // Launch in tmux
            shell_exec("tmux new-session -d -s $tmux_session \"$cmd > $log_file 2>&1\"");
            echo json_encode(["success" => true, "cmd" => $cmd]);
        } else {
            echo json_encode(["error" => "Training via web UI currently only fully supported on Linux/Tmux."]);
        }
        exit;
    }
    
    if ($action === 'stop_train') {
        if ($is_linux) {
            shell_exec("tmux kill-session -t $tmux_session");
        } else {
            shell_exec("taskkill /IM derivbot.exe /F");
        }
        echo json_encode(["success" => true]);
        exit;
    }
    
    if ($action === 'get_logs') {
        if (!file_exists($log_file)) {
            echo json_encode(["logs" => "No active training logs."]);
            exit;
        }
        // Tail last 50 lines
        $logs = shell_exec("tail -n 50 " . escapeshellarg($log_file));
        echo json_encode(["logs" => $logs]);
        exit;
    }
    
    if ($action === 'list_runs') {
        syncResultsToDB($db, $results_dir);
        $stmt = $db->query("SELECT * FROM runs ORDER BY id DESC");
        $runs = $stmt->fetchAll(PDO::FETCH_ASSOC);
        echo json_encode(["runs" => $runs]);
        exit;
    }
    
    if ($action === 'get_run_details') {
        $run_id = $_GET['run_id'] ?? '';
        $symbol = $_GET['symbol'] ?? '';
        $path = "$results_dir/$run_id/$symbol/summary.json";
        if (file_exists($path)) {
            echo file_get_contents($path);
        } else {
            echo json_encode(["error" => "Not found"]);
        }
        exit;
    }
    
    if ($action === 'run_backtest') {
        $run_id = escapeshellarg($_GET['run_id'] ?? '');
        $symbol = escapeshellarg($_GET['symbol'] ?? '');
        $trained_on = escapeshellarg($_GET['trained_on'] ?? '');
        $rank = (int)($_GET['rank'] ?? 1);
        $count = (int)($_GET['count'] ?? 5000);
        $spread = (float)($_GET['spread'] ?? 0.0);
        
        $cmd = "./derivbot --mode backtest --symbol $symbol --load-run $run_id --rank $rank --count $count --spread $spread";
        if ($_GET['trained_on'] ?? '') {
            $cmd .= " --trained-on " . $trained_on;
        }
        $output = shell_exec($cmd . " 2>&1");
        
        echo json_encode(["output" => $output]);
        exit;
    }
    
    if ($action === 'delete_run') {
        $run_id = $_POST['run_id'] ?? '';
        if (!$run_id) { echo json_encode(["error" => "No run_id provided"]); exit; }
        
        // Delete from DB
        $stmt = $db->prepare("DELETE FROM runs WHERE run_id = ?");
        $stmt->execute([$run_id]);
        
        // Delete from filesystem
        $run_dir = $results_dir . '/' . basename($run_id);
        if (is_dir($run_dir)) {
            shell_exec("rm -rf " . escapeshellarg($run_dir));
        }
        
        echo json_encode(["success" => true]);
        exit;
    }
    
    // ====================== LIVE TRADING ENDPOINTS ======================
    if ($action === 'live_status') {
        $status = 'stopped';
        if ($is_linux) {
            $check = shell_exec("tmux has-session -t $live_session 2>&1");
            if (strpos($check, 'no server') === false && strpos($check, "can't find session") === false) {
                $status = 'running';
            }
        }
        echo json_encode(["status" => $status]);
        exit;
    }

    if ($action === 'start_live') {
        if (!$is_linux) {
            echo json_encode(["error" => "Live trading via the dashboard runs on Linux/tmux (your VPS)."]);
            exit;
        }
        $token = trim($_POST['token'] ?? '');
        if ($token === '') { echo json_encode(["error" => "API token is required."]); exit; }

        // Build the command WITHOUT the token (passed via env so it never shows
        // in the process list / JSON response).
        $symbol   = escapeshellarg($_POST['symbol'] ?? 'gold');
        $strategy = escapeshellarg($_POST['strategy'] ?? 'rsi');
        $cmd  = "./derivbot --mode live --symbol $symbol --strategy $strategy";
        $cmd .= " --stake "      . (float)($_POST['stake'] ?? 1);
        $cmd .= " --multiplier " . (int)($_POST['multiplier'] ?? 100);
        if (isset($_POST['sl_amount']))    $cmd .= " --sl-amount "    . (float)$_POST['sl_amount'];
        if (isset($_POST['tp_amount']))    $cmd .= " --tp-amount "    . (float)$_POST['tp_amount'];
        if (isset($_POST['be_amount']))    $cmd .= " --be-amount "    . (float)$_POST['be_amount'];
        if (isset($_POST['trail_amount'])) $cmd .= " --trail-amount " . (float)$_POST['trail_amount'];
        if (isset($_POST['cooldown']))     $cmd .= " --cooldown "     . (int)$_POST['cooldown'];
        if (isset($_POST['max_consec']))   $cmd .= " --max-consec-losses " . (int)$_POST['max_consec'];
        if (isset($_POST['daily_loss']))   $cmd .= " --daily-loss "   . (float)$_POST['daily_loss'];
        $cmd .= " --csv " . escapeshellarg($live_csv);

        if (file_exists($live_log)) unlink($live_log);

        // Pass token through the environment of the tmux session.
        $envToken = escapeshellarg($token);
        shell_exec("tmux new-session -d -s $live_session "
                 . "\"DERIV_TOKEN=$envToken $cmd > $live_log 2>&1\"");

        echo json_encode(["success" => true, "cmd" => $cmd]); // note: token NOT included
        exit;
    }

    if ($action === 'stop_live') {
        if ($is_linux) shell_exec("tmux kill-session -t $live_session 2>&1");
        echo json_encode(["success" => true]);
        exit;
    }

    if ($action === 'get_live_logs') {
        if (!file_exists($live_log)) { echo json_encode(["logs" => "No live session logs yet."]); exit; }
        $logs = shell_exec("tail -n 80 " . escapeshellarg($live_log));
        echo json_encode(["logs" => $logs]);
        exit;
    }

    if ($action === 'live_trades') {
        // Parse the live trade ledger for a monitoring table + running P&L.
        $rows = [];
        $pnl = 0.0; $wins = 0; $total = 0;
        if (file_exists($live_csv) && ($fh = fopen($live_csv, 'r'))) {
            $header = fgetcsv($fh); // open_time,dir,stake,multiplier,close_profit,balance
            while (($r = fgetcsv($fh)) !== false) {
                if (count($r) < 6) continue;
                $profit = (float)$r[4];
                $pnl += $profit; $total++; if ($profit > 0) $wins++;
                $rows[] = [
                    "time"       => (int)$r[0],
                    "dir"        => $r[1],
                    "stake"      => (float)$r[2],
                    "multiplier" => (int)$r[3],
                    "profit"     => $profit,
                    "balance"    => (float)$r[5],
                ];
            }
            fclose($fh);
        }
        $rows = array_slice(array_reverse($rows), 0, 50); // newest first, cap 50
        echo json_encode([
            "trades"   => $rows,
            "sessionPnl" => $pnl,
            "wins"     => $wins,
            "total"    => $total,
            "winRate"  => $total > 0 ? round(100.0 * $wins / $total, 1) : 0,
        ]);
        exit;
    }

    echo json_encode(["error" => "Unknown action"]);
    exit;
}

// ============================================================================
// 3. HTML / Frontend
// ============================================================================
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Ratrix Bot v2 - Dashboard</title>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&display=swap" rel="stylesheet">
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        :root {
            --sidebar-bg: #1a1614;
            --sidebar-text: #8c8582;
            --sidebar-hover: #ffffff;
            --sidebar-active-bg: #2d2623;
            --sidebar-active-text: #e8cfc8;
            --main-bg: #fdfcfb;
            --text-main: #111111;
            --text-muted: #666666;
            --accent: #e55a50;
            --border-color: #e5e5e5;
            --code-bg: #1e1e1e;
            --code-text: #00ff00;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Inter', sans-serif;
            background-color: var(--main-bg);
            color: var(--text-main);
            display: flex;
            height: 100vh;
            overflow: hidden;
            line-height: 1.6;
        }
        .sidebar {
            width: 280px;
            background-color: var(--sidebar-bg);
            color: var(--sidebar-text);
            display: flex;
            flex-direction: column;
            padding: 24px 16px;
            overflow-y: auto;
            flex-shrink: 0;
        }
        .brand {
            font-size: 1.25rem;
            font-weight: 800;
            color: var(--accent);
            margin-bottom: 32px;
            padding-left: 12px;
            letter-spacing: -0.5px;
        }
        .nav-group { margin-bottom: 24px; }
        .nav-title {
            font-size: 0.75rem;
            font-weight: 700;
            text-transform: uppercase;
            letter-spacing: 1px;
            margin-bottom: 12px;
            padding-left: 12px;
            color: #5a524e;
        }
        .nav-item {
            display: block;
            padding: 8px 12px;
            color: var(--sidebar-text);
            text-decoration: none;
            font-size: 0.9rem;
            font-weight: 500;
            border-radius: 6px;
            margin-bottom: 4px;
            transition: all 0.2s ease;
            cursor: pointer;
        }
        .nav-item:hover {
            color: var(--sidebar-hover);
            background-color: rgba(255, 255, 255, 0.05);
        }
        .nav-item.active {
            background-color: var(--sidebar-active-bg);
            color: var(--sidebar-active-text);
            border-radius: 6px;
        }
        .main-content {
            flex: 1;
            overflow-y: auto;
            padding: 48px 80px;
        }
        .view-section { display: none; }
        .view-section.active { display: block; }
        
        h1 { font-size: 2rem; font-weight: 800; margin-bottom: 8px; letter-spacing: -0.5px; }
        h2 { font-size: 1.25rem; font-weight: 700; margin: 32px 0 16px; padding-bottom: 8px; border-bottom: 1px solid var(--border-color); }
        
        .card {
            background: white;
            border: 1px solid var(--border-color);
            border-radius: 8px;
            padding: 24px;
            margin-bottom: 24px;
            box-shadow: 0 1px 3px rgba(0,0,0,0.05);
        }
        .status-badge {
            display: inline-block;
            padding: 4px 12px;
            border-radius: 12px;
            font-size: 0.85rem;
            font-weight: 600;
        }
        .status-running { background: #dcfce7; color: #166534; }
        .status-stopped { background: #f3f4f6; color: #374151; }
        
        .terminal {
            background: var(--code-bg);
            color: var(--code-text);
            font-family: monospace;
            padding: 16px;
            border-radius: 8px;
            height: 400px;
            overflow-y: auto;
            white-space: pre-wrap;
            font-size: 0.85rem;
        }
        
        .form-group { margin-bottom: 16px; }
        label { display: block; font-weight: 600; margin-bottom: 6px; font-size: 0.9rem; }
        input[type="text"], input[type="number"], select {
            width: 100%;
            padding: 8px 12px;
            border: 1px solid var(--border-color);
            border-radius: 6px;
            font-family: 'Inter', sans-serif;
        }
        button {
            background: var(--accent);
            color: white;
            border: none;
            padding: 10px 20px;
            border-radius: 6px;
            font-weight: 600;
            cursor: pointer;
            transition: background 0.2s;
        }
        button:hover { background: #cc4a41; }
        button.btn-danger { background: #dc2626; }
        button.btn-danger:hover { background: #b91c1c; }
        
        table { width: 100%; border-collapse: collapse; margin-top: 16px; }
        th, td { padding: 12px; text-align: left; border-bottom: 1px solid var(--border-color); font-size: 0.9rem; }
        th { font-weight: 600; color: var(--text-muted); }
        tr:hover { background: #f9f9f9; }
        
        .flex-row { display: flex; gap: 16px; }
        .flex-1 { flex: 1; }
    </style>
</head>
<body>

    <div class="sidebar">
        <div class="brand">Ratrix Bot v2</div>
        
        <div class="nav-group">
            <div class="nav-title">Menu</div>
            <a class="nav-item active" onclick="switchView('dashboard')">Dashboard</a>
            <a class="nav-item" onclick="switchView('train')">Train AI</a>
            <a class="nav-item" onclick="switchView('live')">Live Trading</a>
            <a class="nav-item" onclick="switchView('results')">Results & Charts</a>
            <a class="nav-item" href="doc.html" target="_blank">Documentation</a>
        </div>
    </div>

    <div class="main-content">
        
        <!-- DASHBOARD VIEW -->
        <div id="view-dashboard" class="view-section active">
            <h1>Dashboard</h1>
            <p>Monitor your active bot sessions and view overall performance.</p>
            
            <div class="flex-row" style="margin-top: 24px;">
                <div class="card flex-1">
                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 16px;">
                        <div>
                            <strong>Training Status:</strong> 
                            <span id="train-status-badge" class="status-badge status-stopped">Checking...</span>
                        </div>
                        <div>
                            <button class="btn-danger" onclick="stopTraining()">Stop Bot</button>
                        </div>
                    </div>
                    
                    <div class="terminal" id="terminal-logs">Loading logs...</div>
                </div>
                
                <div class="card flex-1">
                    <h3>Recent Performance (Last 10 Runs)</h3>
                    <div style="position: relative; height: 380px; width: 100%; margin-top: 16px;">
                        <canvas id="dashboardChart"></canvas>
                    </div>
                </div>
            </div>
        </div>
        
        <!-- TRAIN VIEW -->
        <div id="view-train" class="view-section">
            <h1>Train AI</h1>
            <p>Start a new reinforcement learning session.</p>
            
            <div class="card" style="margin-top: 24px;">
                <form id="train-form" onsubmit="startTraining(event)">
                    <div class="flex-row">
                        <div class="form-group flex-1">
                            <label>Market / Symbol</label>
                            <select id="train-symbol" onchange="onSymbolChange()">
                                <optgroup label="Synthetic Indices">
                                    <option value="jump10" selected>Jump 10 (JD10)</option>
                                    <option value="jump25">Jump 25 (JD25)</option>
                                    <option value="jump50">Jump 50 (JD50)</option>
                                    <option value="jump75">Jump 75 (JD75)</option>
                                    <option value="jump100">Jump 100 (JD100)</option>
                                    <option value="v10">Volatility 10 (R_10)</option>
                                    <option value="v25">Volatility 25 (R_25)</option>
                                    <option value="v50">Volatility 50 (R_50)</option>
                                    <option value="v75">Volatility 75 (R_75)</option>
                                    <option value="v100">Volatility 100 (R_100)</option>
                                    <option value="boom1000">Boom 1000</option>
                                    <option value="crash1000">Crash 1000</option>
                                    <option value="boom500">Boom 500</option>
                                    <option value="crash500">Crash 500</option>
                                    <option value="bull">Bull Market (RDBULL)</option>
                                    <option value="bear">Bear Market (RDBEAR)</option>
                                </optgroup>
                                <optgroup label="Forex / Gold">
                                    <option value="eurusd">EUR/USD</option>
                                    <option value="gbpusd">GBP/USD</option>
                                    <option value="usdjpy">USD/JPY</option>
                                    <option value="gold">Gold (XAU/USD)</option>
                                    <option value="gbpjpy">GBP/JPY</option>
                                    <option value="audusd">AUD/USD</option>
                                </optgroup>
                            </select>
                        </div>
                        <div class="form-group flex-1">
                            <label>Market Type</label>
                            <select id="train-market-type" onchange="toggleMarketType()">
                                <option value="synthetic">Synthetic Indices</option>
                                <option value="forex">Forex / Gold</option>
                            </select>
                        </div>
                        <div class="form-group flex-1">
                            <label>Historical Ticks to Fetch</label>
                            <input type="number" id="train-history" value="50000" step="1000">
                        </div>
                    </div>
                    
                    <div id="forex-options" style="display:none; background: #f9f9f9; padding: 16px; border-radius: 6px; margin-bottom: 16px;">
                        <div class="flex-row">
                            <div class="form-group flex-1">
                                <label>Take Profit (Pips)</label>
                                <input type="number" id="train-tp" value="10">
                            </div>
                            <div class="form-group flex-1">
                                <label>Stop Loss (Pips)</label>
                                <input type="number" id="train-sl" value="5">
                            </div>
                            <div class="form-group flex-1">
                                <label>Lot Size</label>
                                <input type="text" id="train-lot" value="0.5">
                            </div>
                            <div class="form-group flex-1">
                                <label>Spread (Pips)</label>
                                <input type="number" id="train-spread" value="2.0" step="0.1">
                            </div>
                        </div>
                    </div>
                    
                    <h2>Advanced Search Constraints</h2>
                    <div style="background: #fafafa; border: 1px solid var(--border-color); padding: 16px; border-radius: 6px; margin-bottom: 16px;">
                        <div class="form-group">
                            <label>Allowed Strategies (Check to include. Empty means ALL)</label>
                            <div style="display: flex; gap: 12px; flex-wrap: wrap;">
                                <label style="font-weight: normal;"><input type="checkbox" class="strat-cb" value="rsi"> RSI</label>
                                <label style="font-weight: normal;"><input type="checkbox" class="strat-cb" value="ema_cross"> EMA Cross</label>
                                <label style="font-weight: normal;"><input type="checkbox" class="strat-cb" value="macd"> MACD</label>
                                <label style="font-weight: normal;"><input type="checkbox" class="strat-cb" value="bollinger"> Bollinger</label>
                                <label style="font-weight: normal;"><input type="checkbox" class="strat-cb" value="stochastic"> Stochastic</label>
                                <label style="font-weight: normal;"><input type="checkbox" class="strat-cb" value="multi_confluence"> Confluence</label>
                                <label style="font-weight: normal;"><input type="checkbox" class="strat-cb" value="price_action"> Price Action</label>
                                <label style="font-weight: normal;"><input type="checkbox" class="strat-cb" value="random_forest"> Random Forest (ML)</label>
                                <label style="font-weight: normal;"><input type="checkbox" class="strat-cb" value="xgboost"> XGBoost (ML)</label>
                            </div>
                        </div>
                        
                        <div class="flex-row">
                            <div class="form-group flex-1">
                                <label>Trade Durations (Synthetics)</label>
                                <div style="display: flex; gap: 12px;">
                                    <label style="font-weight: normal;"><input type="checkbox" class="dur-cb" value="10"> 10s</label>
                                    <label style="font-weight: normal;"><input type="checkbox" class="dur-cb" value="15" checked> 15s</label>
                                    <label style="font-weight: normal;"><input type="checkbox" class="dur-cb" value="30"> 30s</label>
                                    <label style="font-weight: normal;"><input type="checkbox" class="dur-cb" value="60"> 1m</label>
                                </div>
                            </div>
                            <div class="form-group flex-1">
                                <label>Candle Periods (Price Action)</label>
                                <div style="display: flex; gap: 12px;">
                                    <label style="font-weight: normal;"><input type="checkbox" class="can-cb" value="5"> 5s</label>
                                    <label style="font-weight: normal;"><input type="checkbox" class="can-cb" value="10"> 10s</label>
                                    <label style="font-weight: normal;"><input type="checkbox" class="can-cb" value="15" checked> 15s</label>
                                    <label style="font-weight: normal;"><input type="checkbox" class="can-cb" value="30"> 30s</label>
                                </div>
                            </div>
                        </div>
                    </div>
                    
                    <h2>Seed Parameters (Optional)</h2>
                    <div class="flex-row">
                        <div class="form-group flex-1">
                            <label>Seed Run ID (e.g. run_20260621_234305)</label>
                            <input type="text" id="train-seed-run" placeholder="Leave blank for fresh start">
                        </div>
                        <div class="form-group flex-1">
                            <label>Seed Rank</label>
                            <input type="number" id="train-seed-rank" value="1">
                        </div>
                    </div>
                    
                    <div class="form-group" style="margin-top: 16px; display: flex; gap: 24px; background: #eef2ff; padding: 12px; border-radius: 6px;">
                        <label>
                            <input type="checkbox" id="train-autoadjust" checked> Run continuous background learning (--autoadjust)
                        </label>
                        <label>
                            <input type="checkbox" id="train-rl" checked> Execute Phase 2 RL/Q-Learning Meta-Learner
                        </label>
                    </div>
                    
                    <button type="submit" style="margin-top: 16px; width: 100%;">Start Training</button>
                </form>
            </div>
        </div>
        
        <!-- LIVE TRADING VIEW -->
        <div id="view-live" class="view-section">
            <h1>Live Trading</h1>
            <p>Trade gold / forex on your Deriv <strong>demo</strong> account via the API (runs on this VPS, no MT5). Login, trade and monitor here.</p>

            <div class="flex-row" style="margin-top: 24px;">
                <div class="card flex-1">
                    <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px;">
                        <div><strong>Status:</strong> <span id="live-status-badge" class="status-badge status-stopped">Checking...</span></div>
                        <div>
                            <span id="live-pnl" style="margin-right:16px; font-weight:600;"></span>
                            <button class="btn-danger" onclick="stopLive()">Stop Bot</button>
                        </div>
                    </div>

                    <form id="live-form" onsubmit="startLive(event)">
                        <div class="form-group">
                            <label>Deriv API Token (DEMO account — Read + Trade scope)</label>
                            <input type="password" id="live-token" placeholder="Paste your demo API token" autocomplete="off">
                            <small style="color:var(--text-muted)">Never stored or shown back; passed to the bot via env.</small>
                        </div>
                        <div class="flex-row">
                            <div class="form-group flex-1">
                                <label>Symbol</label>
                                <select id="live-symbol">
                                    <option value="gold" selected>Gold (XAU/USD)</option>
                                    <option value="eurusd">EUR/USD</option>
                                    <option value="gbpusd">GBP/USD</option>
                                    <option value="usdjpy">USD/JPY</option>
                                    <option value="v75">Volatility 75</option>
                                    <option value="v100">Volatility 100</option>
                                </select>
                            </div>
                            <div class="form-group flex-1">
                                <label>Strategy</label>
                                <select id="live-strategy">
                                    <option value="rsi" selected>RSI rebound (trades immediately)</option>
                                    <option value="random_forest">Random Forest (ML — needs warm-up)</option>
                                    <option value="xgboost">XGBoost (ML — needs warm-up)</option>
                                    <option value="multi_confluence">Multi-Confluence</option>
                                </select>
                            </div>
                        </div>
                        <div class="flex-row">
                            <div class="form-group flex-1"><label>Stake ($)</label><input type="number" id="live-stake" value="1" step="0.1"></div>
                            <div class="form-group flex-1"><label>Multiplier</label><input type="number" id="live-mult" value="100"></div>
                            <div class="form-group flex-1"><label>Stop Loss ($)</label><input type="number" id="live-sl" value="5" step="0.1"></div>
                            <div class="form-group flex-1"><label>Take Profit ($)</label><input type="number" id="live-tp" value="10" step="0.1"></div>
                        </div>
                        <div class="flex-row">
                            <div class="form-group flex-1"><label>Trail arms at ($)</label><input type="number" id="live-be" value="5" step="0.1"></div>
                            <div class="form-group flex-1"><label>Trail giveback ($)</label><input type="number" id="live-trail" value="3" step="0.1"></div>
                            <div class="form-group flex-1"><label>Cooldown (s)</label><input type="number" id="live-cooldown" value="60"></div>
                            <div class="form-group flex-1"><label>Max consec losses</label><input type="number" id="live-maxconsec" value="3"></div>
                            <div class="form-group flex-1"><label>Daily loss cap ($)</label><input type="number" id="live-daily" value="50"></div>
                        </div>
                        <button type="submit" style="width:100%;">Connect & Start Trading</button>
                    </form>
                </div>
            </div>

            <div class="flex-row">
                <div class="card flex-1">
                    <h3>Live Log</h3>
                    <div class="terminal" id="live-logs" style="height:300px;">No live session.</div>
                </div>
                <div class="card flex-1">
                    <h3>Trades <span id="live-winrate" style="font-weight:400; color:var(--text-muted);"></span></h3>
                    <div style="max-height:300px; overflow-y:auto;">
                        <table>
                            <thead><tr><th>Time</th><th>Dir</th><th>Stake</th><th>Profit</th><th>Balance</th></tr></thead>
                            <tbody id="live-trades-body"></tbody>
                        </table>
                    </div>
                </div>
            </div>
        </div>

        <!-- RESULTS VIEW -->
        <div id="view-results" class="view-section">
            <h1>Results & Charts</h1>
            <p>Historical runs from the local SQLite database.</p>
            
            <div class="card" style="margin-top: 24px; display: none;" id="chart-card">
                <div style="display: flex; justify-content: space-between;">
                    <h3 id="chart-title">Run Details</h3>
                    <button onclick="closeChart()">Close Chart</button>
                </div>
                <div style="position: relative; height: 300px; width: 100%; margin-top: 16px;">
                    <canvas id="myChart"></canvas>
                </div>
            </div>
            
            <div class="card" style="margin-top: 24px; display: none;" id="backtest-card">
                <div style="display: flex; justify-content: space-between; margin-bottom: 16px;">
                    <h3 style="margin: 0;">Backtest Configuration & Output</h3>
                    <button onclick="document.getElementById('backtest-card').style.display = 'none'" style="background: #ccc; color: #333;">Close</button>
                </div>
                
                <div id="backtest-config" style="background: #fafafa; border: 1px solid var(--border-color); padding: 16px; border-radius: 6px; margin-bottom: 16px; display: flex; gap: 16px; align-items: center; flex-wrap: wrap;">
                    <div style="flex: 1; min-width: 200px;">
                        <label style="display: block; margin-bottom: 8px; font-weight: 500;">Test Symbol</label>
                        <select id="backtest-symbol-select" style="width: 100%; padding: 8px; border: 1px solid var(--border-color); border-radius: 4px; background: white;">
                            <optgroup label="Synthetic Indices">
                                <option value="jump10">Jump 10 (JD10)</option>
                                <option value="jump25">Jump 25 (JD25)</option>
                                <option value="jump50">Jump 50 (JD50)</option>
                                <option value="jump75">Jump 75 (JD75)</option>
                                <option value="jump100">Jump 100 (JD100)</option>
                                <option value="v10">Volatility 10 (R_10)</option>
                                <option value="v25">Volatility 25 (R_25)</option>
                                <option value="v50">Volatility 50 (R_50)</option>
                                <option value="v75">Volatility 75 (R_75)</option>
                                <option value="v100">Volatility 100 (R_100)</option>
                                <option value="boom1000">Boom 1000</option>
                                <option value="crash1000">Crash 1000</option>
                                <option value="boom500">Boom 500</option>
                                <option value="crash500">Crash 500</option>
                                <option value="bull">Bull Market (RDBULL)</option>
                                <option value="bear">Bear Market (RDBEAR)</option>
                            </optgroup>
                            <optgroup label="Forex / Gold">
                                <option value="eurusd">EUR/USD</option>
                                <option value="gbpusd">GBP/USD</option>
                                <option value="usdjpy">USD/JPY</option>
                                <option value="gold">Gold (XAU/USD)</option>
                                <option value="gbpjpy">GBP/JPY</option>
                                <option value="audusd">AUD/USD</option>
                            </optgroup>
                        </select>
                    </div>
                    <div style="flex: 1; min-width: 200px;">
                        <label style="display: block; margin-bottom: 8px; font-weight: 500;">History Length</label>
                        <select id="backtest-ticks-select" style="width: 100%; padding: 8px; border: 1px solid var(--border-color); border-radius: 4px; background: white;">
                            <option value="5000">5,000 Ticks</option>
                            <option value="10000" selected>10,000 Ticks</option>
                            <option value="50000">50,000 Ticks (~2 weeks)</option>
                            <option value="100000">100,000 Ticks (~1 month)</option>
                        </select>
                    </div>
                    <div style="flex: 1; min-width: 100px;">
                        <label style="display: block; margin-bottom: 8px; font-weight: 500;">Spread</label>
                        <input type="number" id="backtest-spread" value="2.0" step="0.1">
                    </div>
                    <div style="margin-top: 28px;">
                        <button onclick="executeConfiguredBacktest()" style="background: #16a34a; padding: 10px 24px; font-size: 1rem;">Start Backtest</button>
                    </div>
                </div>

                <div class="terminal" id="backtest-output" style="height: 400px; overflow-y: auto;"></div>
            </div>
            
            <div class="card">
                <table>
                    <thead>
                        <tr>
                            <th>Run ID</th>
                            <th>Symbol</th>
                            <th>Best Strategy (Rank 1)</th>
                            <th>Win Rate</th>
                            <th>Grid P&L</th>
                            <th>Agent P&L</th>
                            <th>Action</th>
                        </tr>
                    </thead>
                    <tbody id="results-table-body">
                        <!-- Populated by JS -->
                    </tbody>
                </table>
            </div>

            <!-- RANK EXPLORER MODAL -->
            <div class="card" style="margin-top: 24px; display: none; background: #fafafa;" id="rank-explorer-card">
                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 16px;">
                    <h2 id="rank-explorer-title" style="margin: 0; padding: 0; border: none;">Run Details</h2>
                    <button onclick="document.getElementById('rank-explorer-card').style.display='none'" style="background: #ccc; color: #333;">Close</button>
                </div>
                
                <table style="background: white; border-radius: 8px; overflow: hidden; box-shadow: 0 1px 2px rgba(0,0,0,0.1);">
                    <thead>
                        <tr style="background: #f1f1f1;">
                            <th>Rank</th>
                            <th>Strategy</th>
                            <th>Duration</th>
                            <th>Win Rate</th>
                            <th>Net P&L</th>
                            <th>Max Drawdown</th>
                            <th>Parameters (Indicators)</th>
                            <th>Action</th>
                        </tr>
                    </thead>
                    <tbody id="rank-explorer-body">
                    </tbody>
                </table>
            </div>
        </div>

    </div>

    <script>
        let logInterval = null;
        let dashboardRefreshInterval = null;
        let resultsRefreshInterval = null;
        let liveRefreshInterval = null;
        let chartInstance = null;

        let dashboardChartInstance = null;

        function switchView(viewId) {
            document.querySelectorAll('.view-section').forEach(el => el.classList.remove('active'));
            document.querySelectorAll('.nav-item').forEach(el => el.classList.remove('active'));
            
            document.getElementById('view-' + viewId).classList.add('active');
            if (event && event.target) {
                event.target.classList.add('active');
            }
            
            // Clear all intervals
            if (logInterval) { clearInterval(logInterval); logInterval = null; }
            if (dashboardRefreshInterval) { clearInterval(dashboardRefreshInterval); dashboardRefreshInterval = null; }
            if (resultsRefreshInterval) { clearInterval(resultsRefreshInterval); resultsRefreshInterval = null; }
            if (liveRefreshInterval) { clearInterval(liveRefreshInterval); liveRefreshInterval = null; }

            if (viewId === 'live') {
                refreshLive();
                liveRefreshInterval = setInterval(refreshLive, 3000);
            }

            if (viewId === 'dashboard') {
                checkStatus();
                logInterval = setInterval(fetchLogs, 2000);
                loadDashboardChart();
                dashboardRefreshInterval = setInterval(loadDashboardChart, 10000); // refresh chart every 10s
            }
            
            if (viewId === 'results') {
                loadResults();
                resultsRefreshInterval = setInterval(loadResults, 10000); // refresh list every 10s
            }
        }
        
        async function loadDashboardChart() {
            try {
                const res = await fetch('?action=list_runs');
                const data = await res.json();
                
                const recent = data.runs.slice(0, 10).reverse(); // Last 10 runs
                const labels = recent.map(r => r.run_id.split('_')[1].substring(4) + " " + r.symbol); // short label
                const agentPnl = recent.map(r => r.rl_pnl);
                const bestGridPnl = recent.map(r => r.top_pnl);
                
                if (dashboardChartInstance) dashboardChartInstance.destroy();
                
                const ctx = document.getElementById('dashboardChart').getContext('2d');
                dashboardChartInstance = new Chart(ctx, {
                    type: 'line',
                    data: {
                        labels: labels,
                        datasets: [
                            {
                                label: 'Agent P&L ($)',
                                data: agentPnl,
                                borderColor: '#e55a50',
                                backgroundColor: 'rgba(229, 90, 80, 0.1)',
                                fill: true,
                                tension: 0.3
                            },
                            {
                                label: 'Best Grid P&L ($)',
                                data: bestGridPnl,
                                borderColor: '#333333',
                                backgroundColor: 'transparent',
                                borderDash: [5, 5],
                                tension: 0.3
                            }
                        ]
                    },
                    options: { responsive: true, maintainAspectRatio: false }
                });
            } catch(e) { console.error("Failed to load dashboard chart", e); }
        }

        function toggleMarketType() {
            const isForex = document.getElementById('train-market-type').value === 'forex';
            document.getElementById('forex-options').style.display = isForex ? 'block' : 'none';
        }

        function onSymbolChange() {
            const sym = document.getElementById('train-symbol').value;
            const forexSymbols = ['eurusd', 'gbpusd', 'usdjpy', 'gold', 'gbpjpy', 'audusd'];
            const marketType = document.getElementById('train-market-type');
            if (forexSymbols.includes(sym)) {
                marketType.value = 'forex';
            } else {
                marketType.value = 'synthetic';
            }
            toggleMarketType();
        }

        async function checkStatus() {
            try {
                const res = await fetch('?action=status');
                const data = await res.json();
                const badge = document.getElementById('train-status-badge');
                if (data.status === 'running') {
                    badge.textContent = 'RUNNING';
                    badge.className = 'status-badge status-running';
                } else {
                    badge.textContent = 'STOPPED';
                    badge.className = 'status-badge status-stopped';
                }
            } catch(e) { console.error(e); }
        }

        async function fetchLogs() {
            try {
                const res = await fetch('?action=get_logs');
                const data = await res.json();
                const term = document.getElementById('terminal-logs');
                term.textContent = data.logs;
                term.scrollTop = term.scrollHeight; // Auto-scroll
            } catch(e) { console.error(e); }
        }

        async function startTraining(e) {
            e.preventDefault();
            const symbol = document.getElementById('train-symbol').value;
            const marketType = document.getElementById('train-market-type').value;
            const seedRun = document.getElementById('train-seed-run').value;
            const seedRank = document.getElementById('train-seed-rank').value;
            const autoadjust = document.getElementById('train-autoadjust').checked;
            const rlEnabled = document.getElementById('train-rl').checked;
            const history = document.getElementById('train-history').value;
            
            // Get checkboxes
            const strats = Array.from(document.querySelectorAll('.strat-cb:checked')).map(cb => cb.value).join(',');
            const durs = Array.from(document.querySelectorAll('.dur-cb:checked')).map(cb => cb.value).join(',');
            const cans = Array.from(document.querySelectorAll('.can-cb:checked')).map(cb => cb.value).join(',');
            
            let formData = new FormData();
            formData.append('symbol', symbol);
            formData.append('autoadjust', autoadjust);
            formData.append('no_rl', !rlEnabled);
            if (history) formData.append('history', history);
            if (strats) formData.append('strategies', strats);
            if (durs) formData.append('durations', durs);
            if (cans) formData.append('candles', cans);
            
            if (seedRun) {
                formData.append('seed_run', seedRun);
                formData.append('seed_rank', seedRank);
            }
            if (marketType === 'forex') {
                formData.append('tp', document.getElementById('train-tp').value);
                formData.append('sl', document.getElementById('train-sl').value);
                formData.append('lot', document.getElementById('train-lot').value);
                formData.append('spread', document.getElementById('train-spread').value);
            }

            try {
                const res = await fetch('?action=start_train', { method: 'POST', body: formData });
                const data = await res.json();
                if(data.success) {
                    alert('Training started successfully in background!');
                    switchView('dashboard');
                } else {
                    alert('Error: ' + data.error);
                }
            } catch(e) { alert('Failed to start training'); }
        }

        async function stopTraining() {
            if(!confirm('Are you sure you want to kill the bot process?')) return;
            await fetch('?action=stop_train');
            checkStatus();
        }

        function formatMoney(amount) {
            const num = parseFloat(amount);
            if (num < 0) {
                return `<span style="color: #dc2626; font-weight: 600;">-$${Math.abs(num).toFixed(2)}</span>`;
            } else if (num > 0) {
                return `<span style="color: #16a34a; font-weight: 600;">+$${num.toFixed(2)}</span>`;
            }
            return `$0.00`;
        }

        async function loadResults() {
            try {
                const res = await fetch('?action=list_runs');
                const data = await res.json();
                const tbody = document.getElementById('results-table-body');
                tbody.innerHTML = '';
                
                data.runs.forEach(r => {
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td>${r.run_id}</td>
                        <td>${r.symbol}</td>
                        <td><strong>${r.top_strategy}</strong></td>
                        <td>${parseFloat(r.top_win_rate).toFixed(1)}%</td>
                        <td>${formatMoney(r.top_pnl)}</td>
                        <td>${formatMoney(r.rl_pnl)}</td>
                        <td>
                            <button onclick="viewChart('${r.run_id}', '${r.symbol}')" style="padding:4px 8px; font-size:0.8rem;">Chart</button>
                            <button onclick="exploreRanks('${r.run_id}', '${r.symbol}')" style="padding:4px 8px; font-size:0.8rem; margin-left: 4px; background: #2563eb;">Explore Ranks</button>
                            <button onclick="deleteRun('${r.run_id}')" style="padding:4px 8px; font-size:0.8rem; margin-left: 4px; background: #dc2626;">Delete</button>
                        </td>
                    `;
                    tbody.appendChild(tr);
                });
            } catch(e) { console.error(e); }
        }

        async function viewChart(runId, symbol) {
            document.getElementById('chart-card').style.display = 'block';
            document.getElementById('chart-title').textContent = `${runId} - ${symbol}`;
            
            try {
                const res = await fetch(`?action=get_run_details&run_id=${runId}&symbol=${symbol}`);
                const data = await res.json();
                
                const labels = data.strategies.map(s => s.strategyName + " (Rank " + s.rank + ")");
                const pnlData = data.strategies.map(s => s.netPnl);
                const ddData = data.strategies.map(s => Math.abs(s.maxDrawdown));
                
                if (chartInstance) chartInstance.destroy();
                
                const ctx = document.getElementById('myChart').getContext('2d');
                chartInstance = new Chart(ctx, {
                    type: 'bar',
                    data: {
                        labels: labels,
                        datasets: [
                            {
                                label: 'Net P&L ($)',
                                data: pnlData,
                                backgroundColor: '#e55a50',
                            },
                            {
                                label: 'Max Drawdown ($)',
                                data: ddData,
                                backgroundColor: '#333333',
                            }
                        ]
                    },
                    options: { responsive: true, maintainAspectRatio: false }
                });
                
            } catch(e) { alert("Failed to load details for chart."); }
        }
        
        function closeChart() {
            document.getElementById('chart-card').style.display = 'none';
        }

        async function exploreRanks(runId, symbol) {
            document.getElementById('rank-explorer-card').style.display = 'block';
            document.getElementById('rank-explorer-title').textContent = `Top Strategies: ${runId} - ${symbol}`;
            const tbody = document.getElementById('rank-explorer-body');
            tbody.innerHTML = '<tr><td colspan="8">Loading...</td></tr>';
            
            try {
                const res = await fetch(`?action=get_run_details&run_id=${runId}&symbol=${symbol}`);
                const data = await res.json();
                
                tbody.innerHTML = '';
                data.strategies.forEach(s => {
                    // Extract duration from params or show default
                    let duration = s.params.trade_duration ? s.params.trade_duration + 's' : '15s';
                    
                    // Clean up params string and handle Forex TP/SL duration display
                    let durationStr = '15s';
                    if (s.params.tp_pips && s.params.sl_pips) {
                        durationStr = `TP: ${parseFloat(s.params.tp_pips).toFixed(1)} / SL: ${parseFloat(s.params.sl_pips).toFixed(1)}`;
                    } else if (s.params.trade_duration) {
                        durationStr = s.params.trade_duration + 's';
                    }

                    let paramsStr = Object.keys(s.params).map(k => {
                        if (k === 'trade_duration' || k === 'tp_pips' || k === 'sl_pips') return ''; // don't show these in params column since they are in duration column
                        let val = s.params[k];
                        if (typeof val === 'number') val = val.toFixed(val % 1 === 0 ? 0 : 2);
                        return `<b>${k}</b>: ${val}`;
                    }).filter(x => x !== '').join('<br>');
                    
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td><strong>#${s.rank}</strong></td>
                        <td>${s.strategyName}</td>
                        <td><span style="background: #f0fdf4; color: #166534; padding: 2px 8px; border-radius: 4px; font-weight: 600; font-size: 0.8rem;">${durationStr}</span></td>
                        <td>${parseFloat(s.winRatePct).toFixed(1)}%</td>
                        <td>${formatMoney(s.netPnl)}</td>
                        <td>${formatMoney(s.maxDrawdown)}</td>
                        <td style="max-width: 300px;">${paramsStr}</td>
                        <td>
                            <button onclick="openBacktestModal('${runId}', '${symbol}', ${s.rank})" style="padding:4px 8px; font-size:0.8rem; background: #16a34a;">Backtest</button>
                        </td>
                    `;
                    tbody.appendChild(tr);
                });
            } catch (e) {
                tbody.innerHTML = '<tr><td colspan="8" style="color:red;">Failed to load ranks.</td></tr>';
            }
        }

        let currentBacktestRunId = '';
        let currentBacktestSymbol = '';
        let currentBacktestRank = 1;

        function openBacktestModal(runId, symbol, rank = 1) {
            currentBacktestRunId = runId;
            currentBacktestSymbol = symbol;
            currentBacktestRank = rank;
            
            // Set the symbol dropdown to the originally trained symbol
            document.getElementById('backtest-symbol-select').value = symbol;
            
            const card = document.getElementById('backtest-card');
            const outDiv = document.getElementById('backtest-output');
            
            card.style.display = 'block';
            outDiv.innerHTML = `<span style="color: #666;">Ready to test parameters from ${runId} (Rank ${rank}). Select symbol and history length, then click Start.</span>`;
            
            card.scrollIntoView({ behavior: 'smooth' });
        }

        async function executeConfiguredBacktest() {
            const count = document.getElementById('backtest-ticks-select').value;
            const targetSymbol = document.getElementById('backtest-symbol-select').value;
            const outDiv = document.getElementById('backtest-output');
            
            outDiv.innerHTML = `<span style="color: #666;">Fetching ${count} ticks and running backtest on ${targetSymbol}... Please wait...</span>`;
            outDiv.scrollTop = 0;
            
            try {
                // Pass targetSymbol to the API, but keep loading the parameters from currentBacktestRunId (which was trained on currentBacktestSymbol)
                // Note: The C++ code's `--load-run` naturally loads from `resultsDir/run_id/symbol/summary.json`.
                // Wait, if `--symbol targetSymbol` is passed to C++, `--load-run` will look for `resultsDir/run_id/targetSymbol/summary.json`.
                // If it was trained on JD10, and we test on R_75, it will fail to load unless we pass the seed-symbol.
                // Let's modify the C++ call in PHP to handle this if needed, or we can just pass the currentBacktestSymbol as a new parameter to `?action=run_backtest`.
                const spread = document.getElementById('backtest-spread') ? document.getElementById('backtest-spread').value : 0;
                const res = await fetch(`?action=run_backtest&run_id=${currentBacktestRunId}&symbol=${targetSymbol}&trained_on=${currentBacktestSymbol}&rank=${currentBacktestRank}&count=${count}&spread=${spread}`);
                const data = await res.json();
                outDiv.textContent = data.output || "No output returned.";
                outDiv.scrollTop = outDiv.scrollHeight;
            } catch(e) {
                outDiv.textContent = "Failed to execute backtest. " + e.message;
            }
        }

        async function deleteRun(runId) {
            if (!confirm('Are you sure you want to permanently delete run ' + runId + '?')) return;
            
            let formData = new FormData();
            formData.append('run_id', runId);
            
            try {
                const res = await fetch('?action=delete_run', { method: 'POST', body: formData });
                const data = await res.json();
                if (data.success) {
                    loadResults();
                    loadDashboardChart();
                } else {
                    alert('Error deleting run: ' + data.error);
                }
            } catch(e) { alert('Failed to delete run.'); }
        }

        // ---------------- Live Trading ----------------
        async function startLive(e) {
            e.preventDefault();
            const token = document.getElementById('live-token').value.trim();
            if (!token) { alert('Paste your Deriv DEMO API token first.'); return; }

            const fd = new FormData();
            fd.append('token', token);
            fd.append('symbol',   document.getElementById('live-symbol').value);
            fd.append('strategy', document.getElementById('live-strategy').value);
            fd.append('stake',    document.getElementById('live-stake').value);
            fd.append('multiplier', document.getElementById('live-mult').value);
            fd.append('sl_amount',  document.getElementById('live-sl').value);
            fd.append('tp_amount',  document.getElementById('live-tp').value);
            fd.append('be_amount',  document.getElementById('live-be').value);
            fd.append('trail_amount', document.getElementById('live-trail').value);
            fd.append('cooldown',   document.getElementById('live-cooldown').value);
            fd.append('max_consec', document.getElementById('live-maxconsec').value);
            fd.append('daily_loss', document.getElementById('live-daily').value);

            try {
                const res = await fetch('?action=start_live', { method: 'POST', body: fd });
                const data = await res.json();
                if (data.success) {
                    document.getElementById('live-token').value = ''; // clear from the form
                    alert('Live bot started on the VPS.');
                    refreshLive();
                } else {
                    alert('Error: ' + (data.error || 'unknown'));
                }
            } catch(err) { alert('Failed to start live bot.'); }
        }

        async function stopLive() {
            if (!confirm('Stop the live trading bot?')) return;
            await fetch('?action=stop_live');
            refreshLive();
        }

        async function refreshLive() {
            // status
            try {
                const s = await (await fetch('?action=live_status')).json();
                const badge = document.getElementById('live-status-badge');
                if (s.status === 'running') { badge.textContent = 'RUNNING'; badge.className = 'status-badge status-running'; }
                else { badge.textContent = 'STOPPED'; badge.className = 'status-badge status-stopped'; }
            } catch(e) {}
            // logs
            try {
                const l = await (await fetch('?action=get_live_logs')).json();
                const term = document.getElementById('live-logs');
                term.textContent = l.logs; term.scrollTop = term.scrollHeight;
            } catch(e) {}
            // trades + P&L
            try {
                const t = await (await fetch('?action=live_trades')).json();
                document.getElementById('live-pnl').innerHTML = 'Session P&L: ' + formatMoney(t.sessionPnl);
                document.getElementById('live-winrate').textContent =
                    t.total > 0 ? `(${t.wins}/${t.total} wins, ${t.winRate}%)` : '';
                const tb = document.getElementById('live-trades-body');
                tb.innerHTML = '';
                (t.trades || []).forEach(r => {
                    const d = new Date(r.time * 1000).toLocaleTimeString();
                    const tr = document.createElement('tr');
                    tr.innerHTML = `<td>${d}</td><td>${r.dir}</td><td>$${r.stake}</td>`
                                 + `<td>${formatMoney(r.profit)}</td><td>$${r.balance.toFixed(2)}</td>`;
                    tb.appendChild(tr);
                });
            } catch(e) {}
        }

        // Init
        checkStatus();
        logInterval = setInterval(fetchLogs, 2000);
    </script>
</body>
</html>
