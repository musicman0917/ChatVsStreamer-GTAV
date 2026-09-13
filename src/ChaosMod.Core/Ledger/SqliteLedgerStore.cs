using System;
using System.Collections.Generic;
using System.Data;
using System.Data.SQLite;
using System.IO;
using System.Threading.Tasks;

namespace ChaosMod.Core.Ledger
{
    /// <summary>
    /// SQLite-backed implementation of ILedgerStore. All SQLite access is
    /// serialized through a private lock since System.Data.SQLite connections
    /// are not safe for concurrent use from multiple threads, and chat/EventSub
    /// callbacks can arrive concurrently on background threads.
    /// </summary>
    public sealed class SqliteLedgerStore : ILedgerStore, IDisposable
    {
        private const int SchemaVersion = 1;

        private readonly string _dbPath;
        private readonly object _gate = new object();
        private SQLiteConnection _connection;

        public SqliteLedgerStore(string dbPath)
        {
            _dbPath = dbPath;
        }

        public void Initialize()
        {
            var dir = Path.GetDirectoryName(_dbPath);
            if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                Directory.CreateDirectory(dir);

            var connectionString = new SQLiteConnectionStringBuilder
            {
                DataSource = _dbPath,
                ForeignKeys = true
            }.ConnectionString;

            _connection = new SQLiteConnection(connectionString);
            _connection.Open();

            lock (_gate)
            {
                using (var tx = _connection.BeginTransaction())
                {
                    Execute(@"CREATE TABLE IF NOT EXISTS SchemaVersion (Version INTEGER NOT NULL);", tx);

                    Execute(@"CREATE TABLE IF NOT EXISTS Viewers (
                        ViewerId      TEXT PRIMARY KEY,
                        Login         TEXT NOT NULL,
                        DisplayName   TEXT NOT NULL,
                        FirstSeenUtc  TEXT NOT NULL,
                        LastSeenUtc   TEXT NOT NULL
                    );", tx);

                    Execute(@"CREATE TABLE IF NOT EXISTS Balances (
                        ViewerId          TEXT PRIMARY KEY REFERENCES Viewers(ViewerId),
                        Balance           INTEGER NOT NULL DEFAULT 0,
                        LifetimeEarned    INTEGER NOT NULL DEFAULT 0,
                        LifetimeSpent     INTEGER NOT NULL DEFAULT 0
                    );", tx);

                    Execute(@"CREATE TABLE IF NOT EXISTS Transactions (
                        TransactionId INTEGER PRIMARY KEY AUTOINCREMENT,
                        ViewerId      TEXT NOT NULL REFERENCES Viewers(ViewerId),
                        Delta         INTEGER NOT NULL,
                        BalanceAfter  INTEGER NOT NULL,
                        Reason        TEXT NOT NULL,
                        EffectId      TEXT,
                        Note          TEXT,
                        CreatedUtc    TEXT NOT NULL
                    );", tx);

                    Execute(@"CREATE INDEX IF NOT EXISTS IX_Transactions_ViewerId_CreatedUtc
                        ON Transactions(ViewerId, CreatedUtc);", tx);

                    using (var cmd = new SQLiteCommand("SELECT COUNT(*) FROM SchemaVersion;", _connection, tx))
                    {
                        var count = Convert.ToInt64(cmd.ExecuteScalar());
                        if (count == 0)
                        {
                            using (var insert = new SQLiteCommand("INSERT INTO SchemaVersion (Version) VALUES (@v);", _connection, tx))
                            {
                                insert.Parameters.AddWithValue("@v", SchemaVersion);
                                insert.ExecuteNonQuery();
                            }
                        }
                    }

                    tx.Commit();
                }
            }
        }

        private void Execute(string sql, SQLiteTransaction tx)
        {
            using (var cmd = new SQLiteCommand(sql, _connection, tx))
                cmd.ExecuteNonQuery();
        }

        public Task EnsureAccountAsync(string viewerId, string login, string displayName)
        {
            return Task.Run(() =>
            {
                lock (_gate)
                {
                    var nowIso = DateTime.UtcNow.ToString("o");
                    using (var tx = _connection.BeginTransaction())
                    {
                        using (var upsert = new SQLiteCommand(@"
                            INSERT INTO Viewers (ViewerId, Login, DisplayName, FirstSeenUtc, LastSeenUtc)
                            VALUES (@id, @login, @display, @now, @now)
                            ON CONFLICT(ViewerId) DO UPDATE SET
                                Login = excluded.Login,
                                DisplayName = excluded.DisplayName,
                                LastSeenUtc = excluded.LastSeenUtc;", _connection, tx))
                        {
                            upsert.Parameters.AddWithValue("@id", viewerId);
                            upsert.Parameters.AddWithValue("@login", login);
                            upsert.Parameters.AddWithValue("@display", displayName);
                            upsert.Parameters.AddWithValue("@now", nowIso);
                            upsert.ExecuteNonQuery();
                        }

                        using (var seedBalance = new SQLiteCommand(@"
                            INSERT OR IGNORE INTO Balances (ViewerId, Balance, LifetimeEarned, LifetimeSpent)
                            VALUES (@id, 0, 0, 0);", _connection, tx))
                        {
                            seedBalance.Parameters.AddWithValue("@id", viewerId);
                            seedBalance.ExecuteNonQuery();
                        }

                        tx.Commit();
                    }
                }
            });
        }

        public Task<long> GetBalanceAsync(string viewerId)
        {
            return Task.Run(() =>
            {
                lock (_gate)
                {
                    using (var cmd = new SQLiteCommand("SELECT Balance FROM Balances WHERE ViewerId = @id;", _connection))
                    {
                        cmd.Parameters.AddWithValue("@id", viewerId);
                        var result = cmd.ExecuteScalar();
                        return result == null ? 0L : Convert.ToInt64(result);
                    }
                }
            });
        }

        public Task<long> CreditAsync(string viewerId, long amount, LedgerReason reason, string effectId, string note)
        {
            if (amount < 0) throw new ArgumentOutOfRangeException(nameof(amount), "Credits must be non-negative; use TryDebitAsync for debits.");

            return Task.Run(() =>
            {
                lock (_gate)
                {
                    using (var tx = _connection.BeginTransaction())
                    {
                        using (var update = new SQLiteCommand(@"
                            UPDATE Balances SET Balance = Balance + @amount, LifetimeEarned = LifetimeEarned + @amount
                            WHERE ViewerId = @id;", _connection, tx))
                        {
                            update.Parameters.AddWithValue("@amount", amount);
                            update.Parameters.AddWithValue("@id", viewerId);
                            var rows = update.ExecuteNonQuery();
                            if (rows == 0)
                                throw new InvalidOperationException($"Viewer '{viewerId}' has no account row; call EnsureAccountAsync first.");
                        }

                        long newBalance;
                        using (var read = new SQLiteCommand("SELECT Balance FROM Balances WHERE ViewerId = @id;", _connection, tx))
                        {
                            read.Parameters.AddWithValue("@id", viewerId);
                            newBalance = Convert.ToInt64(read.ExecuteScalar());
                        }

                        InsertTransaction(tx, viewerId, amount, newBalance, reason, effectId, note);
                        tx.Commit();
                        return newBalance;
                    }
                }
            });
        }

        public Task<DebitResult> TryDebitAsync(string viewerId, long amount, LedgerReason reason, string effectId, string note)
        {
            if (amount < 0) throw new ArgumentOutOfRangeException(nameof(amount));

            return Task.Run(() =>
            {
                lock (_gate)
                {
                    using (var tx = _connection.BeginTransaction())
                    {
                        int rows;
                        using (var update = new SQLiteCommand(@"
                            UPDATE Balances SET Balance = Balance - @amount, LifetimeSpent = LifetimeSpent + @amount
                            WHERE ViewerId = @id AND Balance >= @amount;", _connection, tx))
                        {
                            update.Parameters.AddWithValue("@amount", amount);
                            update.Parameters.AddWithValue("@id", viewerId);
                            rows = update.ExecuteNonQuery();
                        }

                        if (rows != 1)
                        {
                            tx.Rollback();
                            var current = GetBalanceNoLock(viewerId);
                            return new DebitResult { Ok = false, NewBalance = current, TransactionId = null };
                        }

                        long newBalance;
                        using (var read = new SQLiteCommand("SELECT Balance FROM Balances WHERE ViewerId = @id;", _connection, tx))
                        {
                            read.Parameters.AddWithValue("@id", viewerId);
                            newBalance = Convert.ToInt64(read.ExecuteScalar());
                        }

                        var txId = InsertTransaction(tx, viewerId, -amount, newBalance, reason, effectId, note);
                        tx.Commit();
                        return new DebitResult { Ok = true, NewBalance = newBalance, TransactionId = txId };
                    }
                }
            });
        }

        private long GetBalanceNoLock(string viewerId)
        {
            using (var cmd = new SQLiteCommand("SELECT Balance FROM Balances WHERE ViewerId = @id;", _connection))
            {
                cmd.Parameters.AddWithValue("@id", viewerId);
                var result = cmd.ExecuteScalar();
                return result == null ? 0L : Convert.ToInt64(result);
            }
        }

        private long InsertTransaction(SQLiteTransaction tx, string viewerId, long delta, long balanceAfter, LedgerReason reason, string effectId, string note)
        {
            using (var insert = new SQLiteCommand(@"
                INSERT INTO Transactions (ViewerId, Delta, BalanceAfter, Reason, EffectId, Note, CreatedUtc)
                VALUES (@id, @delta, @balAfter, @reason, @effectId, @note, @now);
                SELECT last_insert_rowid();", _connection, tx))
            {
                insert.Parameters.AddWithValue("@id", viewerId);
                insert.Parameters.AddWithValue("@delta", delta);
                insert.Parameters.AddWithValue("@balAfter", balanceAfter);
                insert.Parameters.AddWithValue("@reason", reason.ToString());
                insert.Parameters.AddWithValue("@effectId", (object)effectId ?? DBNull.Value);
                insert.Parameters.AddWithValue("@note", (object)note ?? DBNull.Value);
                insert.Parameters.AddWithValue("@now", DateTime.UtcNow.ToString("o"));
                return Convert.ToInt64(insert.ExecuteScalar());
            }
        }

        public Task<IReadOnlyList<LedgerEntry>> GetHistoryAsync(string viewerId, int take)
        {
            return Task.Run(() =>
            {
                var results = new List<LedgerEntry>();
                lock (_gate)
                {
                    using (var cmd = new SQLiteCommand(@"
                        SELECT TransactionId, ViewerId, Delta, BalanceAfter, Reason, EffectId, Note, CreatedUtc
                        FROM Transactions WHERE ViewerId = @id
                        ORDER BY TransactionId DESC LIMIT @take;", _connection))
                    {
                        cmd.Parameters.AddWithValue("@id", viewerId);
                        cmd.Parameters.AddWithValue("@take", take);
                        using (var reader = cmd.ExecuteReader())
                        {
                            while (reader.Read())
                            {
                                results.Add(new LedgerEntry
                                {
                                    TransactionId = reader.GetInt64(0),
                                    ViewerId = reader.GetString(1),
                                    Delta = reader.GetInt64(2),
                                    BalanceAfter = reader.GetInt64(3),
                                    Reason = (LedgerReason)Enum.Parse(typeof(LedgerReason), reader.GetString(4)),
                                    EffectId = reader.IsDBNull(5) ? null : reader.GetString(5),
                                    Note = reader.IsDBNull(6) ? null : reader.GetString(6),
                                    CreatedUtc = DateTime.Parse(reader.GetString(7)).ToUniversalTime()
                                });
                            }
                        }
                    }
                }
                return (IReadOnlyList<LedgerEntry>)results;
            });
        }

        public Task<IReadOnlyList<LeaderboardRow>> GetLeaderboardAsync(int take)
        {
            return Task.Run(() =>
            {
                var results = new List<LeaderboardRow>();
                lock (_gate)
                {
                    using (var cmd = new SQLiteCommand(@"
                        SELECT v.ViewerId, v.Login, v.DisplayName, b.Balance
                        FROM Balances b JOIN Viewers v ON v.ViewerId = b.ViewerId
                        ORDER BY b.Balance DESC LIMIT @take;", _connection))
                    {
                        cmd.Parameters.AddWithValue("@take", take);
                        using (var reader = cmd.ExecuteReader())
                        {
                            var rank = 1;
                            while (reader.Read())
                            {
                                results.Add(new LeaderboardRow
                                {
                                    Rank = rank++,
                                    ViewerId = reader.GetString(0),
                                    Login = reader.GetString(1),
                                    DisplayName = reader.GetString(2),
                                    Balance = reader.GetInt64(3)
                                });
                            }
                        }
                    }
                }
                return (IReadOnlyList<LeaderboardRow>)results;
            });
        }

        public void Dispose()
        {
            _connection?.Dispose();
        }
    }
}
