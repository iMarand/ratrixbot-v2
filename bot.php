<?php
// Ratrix Bot v2 - Web Dashboard
// Single-file backend & frontend.

session_start();

$db_file = __DIR__ . '/bot.sqlite';
$results_dir = __DIR__ . '/results';
$is_linux = (PHP_OS_FAMILY === 'Linux');
$tmux_session = 'ratrix_train';
$log_file = __DIR__ . '/train.log';

// ============================================================================
// 1. Database Initialization
// ============================================================================
try {
    $db = new PDO('sqlite:' . $db_file);
    $db->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    
    // Create tables if they don't exist
    $db->exec("CREATE TABLE IF NOT EXISTS runs (
        id TEXT PRIMARY KEY,
        symbol TEXT,
        created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
        top_strategy TEXT,
        top_win_rate REAL,
        top_pnl REAL,
        rl_pnl REAL,
        is_synced INTEGER DEFAULT 1
    )");
    
} catch(PDOException $e) {
    die(json_encode(["error" => "Database connection failed: " . $e->getMessage()]));
}

// Helper: Sync results folder into SQLite
function syncResultsToDB($db, $results_dir) {
    if (!is_dir($results_dir)) return;
    
    $run_dirs = glob($results_dir . '/run_*', GLOB_ONLYDIR);
    $existing_runs_stmt = $db->query("SELECT id FROM runs");
    $existing = [];
    while ($row = $existing_runs_stmt->fetch(PDO::FETCH_ASSOC)) {
        $existing[] = $row['id'];
    }
    
    foreach ($run_dirs as $run_dir) {
        $run_id = basename($run_dir);
        if (in_array($run_id, $existing)) continue; // Already synced
        
        // Find symbols inside this run
        $symbol_dirs = glob($run_dir . '/*', GLOB_ONLYDIR);
        foreach ($symbol_dirs as $sym_dir) {
            $symbol = basename($sym_dir);
            $summary_path = $sym_dir . '/summary.json';
            
            if (file_exists($summary_path)) {
                $json = json_decode(file_get_contents($summary_path), true);
                if (!$json) continue;
                
                $top_strat = isset($json['strategies'][0]) ? $json['strategies'][0]['strategyName'] : 'N/A';
                $top_wr = isset($json['strategies'][0]) ? $json['strategies'][0]['winRatePct'] : 0;
                $top_pnl = isset($json['strategies'][0]) ? $json['strategies'][0]['netPnl'] : 0;
                $rl_pnl = isset($json['rl_result']['netPnl']) ? $json['rl_result']['netPnl'] : 0;
                
                $stmt = $db->prepare("INSERT INTO runs (id, symbol, top_strategy, top_win_rate, top_pnl, rl_pnl) VALUES (?, ?, ?, ?, ?, ?)");
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
            if (isset($_POST['autoadjust']) && $_POST['autoadjust'] == 'true') {
                $cmd .= " --autoadjust";
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
        $rank = (int)($_GET['rank'] ?? 1);
        
        $cmd = "./derivbot --mode backtest --symbol $symbol --load-run $run_id --rank $rank";
        $output = shell_exec($cmd . " 2>&1");
        
        echo json_encode(["output" => $output]);
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
            <a class="nav-item" onclick="switchView('results')">Results & Charts</a>
            <a class="nav-item" href="doc.html" target="_blank">Documentation</a>
        </div>
    </div>

    <div class="main-content">
        
        <!-- DASHBOARD VIEW -->
        <div id="view-dashboard" class="view-section active">
            <h1>Dashboard</h1>
            <p>Monitor your active bot sessions.</p>
            
            <div class="card" style="margin-top: 24px;">
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
                            <input type="text" id="train-symbol" value="jump10" required>
                        </div>
                        <div class="form-group flex-1">
                            <label>Market Type</label>
                            <select id="train-market-type" onchange="toggleMarketType()">
                                <option value="synthetic">Synthetic Indices</option>
                                <option value="forex">Forex / Gold</option>
                            </select>
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
                    
                    <div class="form-group" style="margin-top: 16px;">
                        <label>
                            <input type="checkbox" id="train-autoadjust" checked> Auto-adjust capabilities
                        </label>
                    </div>
                    
                    <button type="submit" style="margin-top: 16px; width: 100%;">Start Training</button>
                </form>
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
                <canvas id="myChart" style="width: 100%; height: 300px; margin-top: 16px;"></canvas>
            </div>
            
            <div class="card" style="margin-top: 24px; display: none;" id="backtest-card">
                <div style="display: flex; justify-content: space-between;">
                    <h3>Backtest Output</h3>
                    <button onclick="document.getElementById('backtest-card').style.display = 'none'">Close</button>
                </div>
                <div class="terminal" id="backtest-output" style="height: 300px; margin-top: 16px;"></div>
            </div>
            
            <div class="card">
                <table>
                    <thead>
                        <tr>
                            <th>Run ID</th>
                            <th>Symbol</th>
                            <th>Top Strategy</th>
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
        </div>

    </div>

    <script>
        let logInterval = null;
        let chartInstance = null;

        function switchView(viewId) {
            document.querySelectorAll('.view-section').forEach(el => el.classList.remove('active'));
            document.querySelectorAll('.nav-item').forEach(el => el.classList.remove('active'));
            
            document.getElementById('view-' + viewId).classList.add('active');
            event.target.classList.add('active');
            
            if (viewId === 'dashboard') {
                checkStatus();
                if(!logInterval) logInterval = setInterval(fetchLogs, 2000);
            } else {
                if(logInterval) { clearInterval(logInterval); logInterval = null; }
            }
            
            if (viewId === 'results') {
                loadResults();
            }
        }

        function toggleMarketType() {
            const isForex = document.getElementById('train-market-type').value === 'forex';
            document.getElementById('forex-options').style.display = isForex ? 'block' : 'none';
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
            
            let formData = new FormData();
            formData.append('symbol', symbol);
            formData.append('autoadjust', autoadjust);
            if (seedRun) {
                formData.append('seed_run', seedRun);
                formData.append('seed_rank', seedRank);
            }
            if (marketType === 'forex') {
                formData.append('tp', document.getElementById('train-tp').value);
                formData.append('sl', document.getElementById('train-sl').value);
                formData.append('lot', document.getElementById('train-lot').value);
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

        async function loadResults() {
            try {
                const res = await fetch('?action=list_runs');
                const data = await res.json();
                const tbody = document.getElementById('results-table-body');
                tbody.innerHTML = '';
                
                data.runs.forEach(r => {
                    const tr = document.createElement('tr');
                    tr.innerHTML = `
                        <td>${r.id}</td>
                        <td>${r.symbol}</td>
                        <td>${r.top_strategy}</td>
                        <td>${r.top_win_rate}%</td>
                        <td>$${r.top_pnl}</td>
                        <td>$${r.rl_pnl}</td>
                        <td>
                            <button onclick="viewChart('${r.id}', '${r.symbol}')" style="padding:4px 8px; font-size:0.8rem;">Chart</button>
                            <button onclick="runBacktest('${r.id}', '${r.symbol}')" style="padding:4px 8px; font-size:0.8rem; margin-left: 4px; background: #555;">Backtest</button>
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

        async function runBacktest(runId, symbol) {
            const outDiv = document.getElementById('backtest-output');
            const card = document.getElementById('backtest-card');
            
            card.style.display = 'block';
            outDiv.textContent = `Running backtest for ${symbol} using ${runId} (Rank 1). Please wait...`;
            outDiv.scrollTop = 0;
            
            try {
                const res = await fetch(`?action=run_backtest&run_id=${runId}&symbol=${symbol}&rank=1`);
                const data = await res.json();
                outDiv.textContent = data.output || "No output returned.";
                outDiv.scrollTop = outDiv.scrollHeight;
            } catch(e) {
                outDiv.textContent = "Failed to execute backtest. " + e.message;
            }
        }

        // Init
        checkStatus();
        logInterval = setInterval(fetchLogs, 2000);
    </script>
</body>
</html>
