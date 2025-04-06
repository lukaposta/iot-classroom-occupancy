require('dotenv').config();

const express = require('express');
const sql = require('mssql');
const server = express();
const port = 4000;

const dbConfig = {
    user: process.env.DB_USER,
    password: process.env.DB_PASSWORD,
    server: process.env.DB_SERVER,
    database: process.env.DB_NAME,
    options: {
        encrypt: true,
        enableArithAbort: true
    }
};

const poolPromise = new sql.ConnectionPool(dbConfig)
    .connect()
    .then(pool => {
        console.log("Connected to SQL Server");
        return pool;
    })
    .catch(err => {
        console.error("Database connection failed! Error:", err);
        process.exit(1); // Ako se ne uspije povezati, zaustavi aplikaciju
    });

server.use(express.urlencoded({ extended: true }));
server.use(express.json());
server.use(express.static(__dirname));

server.get('/api/last-status', async (req, res) => {
    try {
        const pool = await poolPromise;
        const result = await pool.request().query(`SELECT TOP 1 Status FROM Telemetry ORDER BY ID DESC`);
        
        console.log("DB Result:", result.recordset);
        res.json({ occupied: result.recordset[0]?.Status ?? null });
    } catch (error) {
        console.error("Error fetching last status:", error); // Log cijele greške
        res.status(500).json({ 
            error: 'Error fetching last status', 
            details: error.message // Dodaj detalje greške
        });
    }
});

server.get('/api/last-occupied-time', async (req, res) => {
    try {
        const pool = await poolPromise;  // Koristi connection pool
        const result = await pool.request().query(`
            SELECT TOP 1 CONVERT(VARCHAR, Timestamp, 126) AS LastOccupiedTimeUTC
            FROM Telemetry
            WHERE Status = 1
            ORDER BY Timestamp DESC
        `);
        
        res.json({ lastOccupiedTime: result.recordset[0]?.LastOccupiedTimeUTC ?? null });
    } catch (error) {
        console.error("Error fetching last occupied time:", error);
        res.status(500).json({ error: 'Error fetching last occupied time' });
    }
});

server.get('/api/peak-time', async (req, res) => {
    try {
        const pool = await poolPromise;
        const result = await pool.request().query(`
            SELECT DATEPART(HOUR, Timestamp) AS Hour, COUNT(*) AS Count
            FROM Telemetry 
            WHERE Status = 0
            GROUP BY DATEPART(HOUR, Timestamp)
            ORDER BY Hour ASC
        `);
        res.json(result.recordset);
    } catch (error) {
        console.error("Error fetching peak time:", error);
        res.status(500).json({ 
            error: 'Error fetching peak time',
            details: error.message
        });
    }
});

server.get('/api/average-occupancy', async (req, res) => {
    try {
        const pool = await poolPromise;
        const result = await pool.request().query(`
            WITH CTE AS (
                SELECT 
                    t1.Timestamp AS StartTime,
                    MIN(t2.Timestamp) AS EndTime
                FROM Telemetry t1
                JOIN Telemetry t2 
                    ON t1.ID < t2.ID 
                    AND t1.Status = 1 
                    AND t2.Status = 0
                    AND t2.Timestamp > t1.Timestamp
                GROUP BY t1.Timestamp
            )
            SELECT AVG(DATEDIFF(SECOND, StartTime, EndTime)) AS AvgTimeSeconds FROM CTE;
        `);
        res.json({ avg_time_seconds: result.recordset[0]?.AvgTimeSeconds ?? "N/A" });
    } catch (error) {
        console.error("Error fetching average occupancy:", error);
        res.status(500).json({ 
            error: 'Error fetching average occupancy',
            details: error.message
        });
    }
});

server.get('/api/daily-summary', async (req, res) => {
    const { date } = req.query;
    if (!date) {
        return res.status(400).json({ error: "Date parameter is required" });
    }

    try {
        const pool = await poolPromise;  // Koristi connection pool
        const request = pool.request();
        request.input('date', sql.Date, date);

        const result = await request.query(`
            WITH Occupancy AS (
                SELECT 
                    Timestamp,
                    Status,
                    LEAD(Timestamp) OVER (ORDER BY Timestamp) AS NextTimestamp
                FROM Telemetry
                WHERE CAST(Timestamp AS DATE) = @date
            )
            SELECT 
                SUM(DATEDIFF(MINUTE, Timestamp, NextTimestamp)) AS TotalOccupiedMinutes
            FROM Occupancy
            WHERE Status = 0
        `);

        res.json({ totalOccupiedMinutes: result.recordset[0]?.TotalOccupiedMinutes ?? 0 });
    } catch (error) {
        console.error("Error fetching daily summary:", error);
        res.status(500).json({ error: "Error fetching daily summary" });
    }
});

server.listen(port, () => {
    console.log(`Server is running on http://localhost:${port}`);
});