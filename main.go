package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"log"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"github.com/gofiber/fiber/v3"
	"github.com/gofiber/fiber/v3/middleware/cors"
	"github.com/gofiber/fiber/v3/middleware/static"
)

// Konfigurasi MQTT sesuai kode ESP32
const (
	MqttBroker  = "tcp://broker.hivemq.com:1883"
	TopicPrefix = "conveyortesst"
	TopicData   = TopicPrefix + "/data"
	TopicCtrl   = TopicPrefix + "/control"
	TopicStatus = TopicPrefix + "/status"
	TopicSerial = TopicPrefix + "/serial"
)

// Payload struktur dari/ke ESP32
type ConveyorData struct {
	Masuk  uint64  `json:"masuk"`
	Normal uint64  `json:"normal"`
	Reject uint64  `json:"reject"`
	Tinggi float64 `json:"tinggi"`
	Ldr    int     `json:"ldr"`
	Hasil  string  `json:"hasil"`
}

type ConveyorStatus struct {
	State         string `json:"state"`
	MqttConnected bool   `json:"mqtt_connected"`
}

type ControlCommand struct {
	Cmd   string `json:"cmd"`
	State string `json:"state"`
}

type SerialLog struct {
	Time    time.Time `json:"time"`
	Message string    `json:"message"`
}

// State Manager & SSE Broadcaster
type Hub struct {
	mu         sync.RWMutex
	data       ConveyorData
	status     ConveyorStatus
	serialLogs []SerialLog
	clients    map[chan string]bool
}

func NewHub() *Hub {
	return &Hub{
		data:    ConveyorData{Hasil: "-"},
		status:  ConveyorStatus{State: "off", MqttConnected: false},
		clients: make(map[chan string]bool),
	}
}

func (h *Hub) AddSerialLog(message string) {
	entry := SerialLog{Time: time.Now(), Message: message}

	h.mu.Lock()
	h.serialLogs = append(h.serialLogs, entry)
	if len(h.serialLogs) > 100 {
		h.serialLogs = h.serialLogs[len(h.serialLogs)-100:]
	}
	h.mu.Unlock()

	h.Broadcast("serial", entry)
}

func (h *Hub) Broadcast(eventType string, payload interface{}) {
	bytes, err := json.Marshal(payload)
	if err != nil {
		return
	}
	msg := fmt.Sprintf("event: %s\ndata: %s\n\n", eventType, string(bytes))

	h.mu.RLock()
	defer h.mu.RUnlock()
	for ch := range h.clients {
		select {
		case ch <- msg:
		default: // Lewati jika channel client penuh
		}
	}
}

func main() {
	hub := NewHub()

	// 1. Setup MQTT Client
	opts := mqtt.NewClientOptions().
		AddBroker(MqttBroker).
		SetClientID(fmt.Sprintf("fiber-dashboard-%d", time.Now().UnixNano())).
		SetAutoReconnect(true).
		SetOnConnectHandler(func(c mqtt.Client) {
			log.Println("Terhubung ke MQTT Broker:", MqttBroker)

			// Subscribe topik data QC
			c.Subscribe(TopicData, 0, func(_ mqtt.Client, m mqtt.Message) {
				var d ConveyorData
				if err := json.Unmarshal(m.Payload(), &d); err == nil {
					hub.mu.Lock()
					hub.data = d
					hub.mu.Unlock()
					hub.Broadcast("data", d)
				}
			})

			// Subscribe topik status conveyor (retained)
			c.Subscribe(TopicStatus, 0, func(_ mqtt.Client, m mqtt.Message) {
				var s ConveyorStatus
				if err := json.Unmarshal(m.Payload(), &s); err == nil {
					hub.mu.Lock()
					hub.status = s
					hub.mu.Unlock()
					hub.Broadcast("status", s)
				}
			})

			c.Subscribe(TopicSerial, 0, func(_ mqtt.Client, m mqtt.Message) {
				hub.AddSerialLog(string(m.Payload()))
			})
		})

	mqttClient := mqtt.NewClient(opts)
	if token := mqttClient.Connect(); token.Wait() && token.Error() != nil {
		log.Fatalf("Gagal konek MQTT: %v", token.Error())
	}

	// 2. Setup Fiber v3
	app := fiber.New()
	app.Use(cors.New())

	// Sajikan file frontend statis dari folder ./public
	app.Get("/*", static.New("./public"))

	// Endpoint untuk mendapatkan state awal saat web dibuka
	app.Get("/api/state", func(c fiber.Ctx) error {
		hub.mu.RLock()
		defer hub.mu.RUnlock()
		return c.JSON(fiber.Map{
			"data":   hub.data,
			"status": hub.status,
			"serial": hub.serialLogs,
		})
	})

	// Endpoint kontrol conveyor (publish ke topik control)
	app.Post("/api/control", func(c fiber.Ctx) error {
		var req struct {
			State string `json:"state"`
		}
		if err := c.Bind().Body(&req); err != nil {
			return c.Status(fiber.StatusBadRequest).JSON(fiber.Map{"error": "Payload tidak valid"})
		}

		if req.State != "on" && req.State != "off" {
			req.State = "off"
		}

		cmd := ControlCommand{
			Cmd:   "conveyor",
			State: req.State,
		}
		payload, _ := json.Marshal(cmd)

		token := mqttClient.Publish(TopicCtrl, 0, false, payload)
		token.Wait()
		if token.Error() != nil {
			return c.Status(fiber.StatusInternalServerError).JSON(fiber.Map{"error": "Gagal publish MQTT"})
		}

		return c.JSON(fiber.Map{
			"message": "Perintah terkirim",
			"command": cmd,
		})
	})

	// Endpoint Server-Sent Events (SSE) untuk update real-time
	app.Get("/events", func(c fiber.Ctx) error {
		c.Set("Content-Type", "text/event-stream")
		c.Set("Cache-Control", "no-cache")
		c.Set("Connection", "keep-alive")

		clientChan := make(chan string, 10)
		hub.mu.Lock()
		hub.clients[clientChan] = true
		hub.mu.Unlock()

		return c.SendStreamWriter(func(w *bufio.Writer) {
			defer func() {
				hub.mu.Lock()
				delete(hub.clients, clientChan)
				hub.mu.Unlock()
				close(clientChan)
			}()

			// Kirim ping awal agar koneksi langsung terbuka
			fmt.Fprintf(w, ": connected\n\n")
			w.Flush()

			for msg := range clientChan {
				if _, err := fmt.Fprint(w, msg); err != nil {
					return
				}
				if err := w.Flush(); err != nil {
					return
				}
			}
		})
	})

	log.Println("Dashboard berjalan di http://localhost:3000")
	log.Fatal(app.Listen(":3000"))
}
