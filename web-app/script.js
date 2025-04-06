async function fetchRoomStatus() {
    try {
        const response = await fetch('/api/last-status');
        const data = await response.json();

        const statusBox = document.getElementById("status-box");
        const averageOccupancyDurationBox = document.getElementById("average-occupancy-duration");
        const occupancyDurationBox = document.getElementById("occupancy-duration");

        if (statusBox) {
            if (data.occupied) {
                statusBox.innerHTML = 'Current status: <strong style="color: red;">Occupied</strong>';
                statusBox.style.backgroundColor = "rgba(248, 215, 218, 1)";
                statusBox.style.borderColor = "rgb(231, 130, 140)";

                const occupiedTimeResponse = await fetch('/api/last-occupied-time');
                const occupiedTimeData = await occupiedTimeResponse.json();
                
                if (occupiedTimeData.lastOccupiedTime) {
                    const lastOccupiedTime = new Date(occupiedTimeData.lastOccupiedTime);
                    const currentTime = new Date();
                    console.log("Last Occupied (UTC):", lastOccupiedTime.toISOString());
                    console.log("Current Time (Local):", currentTime.toISOString());
                    const timeDifference = currentTime - lastOccupiedTime;
                    
                    const minutesOccupied = Math.floor(timeDifference / (1000 * 60));
                    occupancyDurationBox.innerText = `Room has been occupied for: ${minutesOccupied} min`;
                    
                    if (timeDifference / (1000 * 60 * 60) >= 5) { 
                        const popup = document.getElementById("popup");
                        const closePopup = document.querySelector(".close-btn");
                        popup.style.display = "flex";
                    
                        closePopup.addEventListener("click", function () {
                            popup.style.display = "none";
                        });
                    
                        window.addEventListener("click", function (event) {
                            if (event.target === popup) {
                                popup.style.display = "none";
                            }
                        });
                    }
                } else {
                    occupancyDurationBox.innerText = `Error calculating occupancy duration.`;
                }
            } else {
                statusBox.innerHTML = 'Current status: <strong style="color: green;">Available</strong>';
                statusBox.style.backgroundColor = "rgba(212, 237, 218, 1)";
                statusBox.style.borderColor = "rgb(156, 212, 169)";
                occupancyDurationBox.innerText = "";
            }
        }

        const averageOccupancyResponse = await fetch('/api/average-occupancy');
        const averageOccupancyData = await averageOccupancyResponse.json();

        if (averageOccupancyDurationBox) {
            if (averageOccupancyData.avg_time_seconds) {
                const avgTimeMinutes = Math.round(averageOccupancyData.avg_time_seconds / 60);
                averageOccupancyDurationBox.innerText = `Average Occupancy Duration: ${avgTimeMinutes} min`;
            } else {
                averageOccupancyDurationBox.innerText = `Average Occupancy Duration: N/A`;
            }
        }
    } catch (error) {
        console.error("Error fetching room status:", error);
        if (statusBox) {
            statusBox.innerHTML = "Error loading status.";
            statusBox.style.backgroundColor = "rgba(255, 255, 0, 0.2)";
        }
    }
}

let peakTimeChart;

async function fetchPeakTime() {
    try {
        const response = await fetch('/api/peak-time');
        const data = await response.json();

        const sortedData = data.sort((a, b) => a.Hour - b.Hour);
        console.log("Sorted data:", sortedData);

        const peakTimeElement = document.getElementById("peak-time-chart");
        if (peakTimeElement) {
            const ctx = peakTimeElement.getContext('2d');

            if (peakTimeChart) {
                peakTimeChart.destroy();
            }

            const hours = data.map(item => `${item.Hour}:00`);
            const counts = data.map(item => item.Count);

            peakTimeChart = new Chart(ctx, {
                type: 'bar',
                data: {
                    labels: hours,
                    datasets: [{
                        label: 'Occupancy Count',
                        data: counts,
                        backgroundColor: 'rgba(75, 130, 192, 0.5)',
                        borderColor: 'rgb(68, 129, 219)',
                        borderWidth: 1
                    }]
                },
                options: {
                    responsive: true,
                    scales: {
                        x: {
                            title: {
                                display: true,
                                text: 'Hour'
                            }
                        },
                        y: {
                            title: {
                                display: true,
                                text: 'Occupancy Count'
                            },
                            beginAtZero: true
                        }
                    }
                }
            });
        }
    } catch (error) {
        console.error("Error fetching peak time:", error);
        document.getElementById("peak-time-chart").innerText = "Error loading data.";
    }
}

let dailyChart;

async function fetchDailySummary() {
    const selectedDate = document.getElementById("date-picker").value;
    if (!selectedDate) return;

    try {
        const response = await fetch(`/api/daily-summary?date=${selectedDate}`);
        const data = await response.json();

        const totalOccupied = data.totalOccupiedMinutes;
        const totalAvailable = 1440 - totalOccupied;

        const ctx = document.getElementById("daily-summary-chart").getContext("2d");

        if (dailyChart) {
            dailyChart.destroy();
        }

        dailyChart = new Chart(ctx, {
            type: "pie",
            data: {
                labels: ["Occupied", "Available"],
                datasets: [{
                    data: [totalOccupied, totalAvailable],
                    backgroundColor: ["#FF6384", "#36A2EB"],
                    hoverBackgroundColor: ["#FF4364", "#2196F3"]
                }]
            },
            options: {
                responsive: false,
                maintainAspectRatio: false
            }
        });        
    } catch (error) {
        console.error("Error fetching daily summary:", error);
    }
}

window.onload = function () {
    if (document.getElementById("status-box")) {
        fetchRoomStatus();
    }
    if (document.getElementById("peak-time-chart")) {
        fetchPeakTime();
    }
    const datePicker = document.getElementById("date-picker");
    if (datePicker) {
        datePicker.addEventListener("change", fetchDailySummary);
    }
};