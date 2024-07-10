const express = require('express');
const app = express();
const httpServer = require('http').createServer(app); 
const WebSocket = require('ws');
const spawn = require('child_process').spawn;
const fs = require('fs');
const cors = require('cors');
const MediaDevices = require('media-devices');
const axios = require('axios').default;

app.use(cors());
app.use(express.json({extended:false}));
app.use(express.static('public',{}));

const role = "LOCAL_SERVER"
const remoteServerURL = process.argv[2];
const websocketServerURL = process.argv[3];
const laboratoryId = process.argv[4];
const password  = process.argv[5];
const port = process.argv[6];
const timoutReconnect = 5000;

let ws;
let timeoutReconnectHandler;
let token;

app.get('/remote-server-data', function(request, response){
	response.status(200).json({remoteServerURL: remoteServerURL, websocketServerURL: websocketServerURL, laboratoryId: laboratoryId, password:password});
});

app.get('/newfirmware', async function(request, response){
    return response.sendFile(__dirname+'/public/control-firmware/build/app-template.bin');
});
async function start() {
	  try {
		const data = {
			laboratoryId: laboratoryId,
			role: role,
			password: password
		};
		const response = await axios.post(remoteServerURL+'/ws/laboratory/authenticate',data);
		token = response.data.token;
		await wsConnect();
	  } catch (error) {
		console.error(error);
		throw error;
	  }
}
async function wsConnect(){
	if(timeoutReconnectHandler) {
		await clearTimeout(timeoutReconnectHandler);
		timeoutReconnectHandler = null;
	}

	ws = new WebSocket(websocketServerURL);
	ws.on('open', function open() {
		console.log("Conectado ao servidor remoto");
		const data = {
			messageType:"JOIN_LABORATORY",
			role:"LOCAL_SERVER",
			token: token,
			laboratoryId:laboratoryId
        }
		ws.send(JSON.stringify(data));
	});

	ws.on('close', async function er(){
		console.log("CLOSE - Reconectando ao servidor remoto");
		if(!timeoutReconnectHandler){
			await clearTimeout(timeoutReconnectHandler);
			timeoutReconnectHandler = setTimeout(wsConnect,timoutReconnect);
		}
	});

	ws.on('error', async function er(){
		console.log("ERROR - Reconectando ao servidor remoto");
		if(!timeoutReconnectHandler){
			await clearTimeout(timeoutReconnectHandler);
			timeoutReconnectHandler = setTimeout(wsConnect,timoutReconnect);
		}
	});

	ws.on('message', function message(data) {
		const message = JSON.parse(data.toString());

		if(message.messageType === 'JOIN_LABORATORY_OK'){
			console.log(message);
		}

		if(message.messageType !== 'SOURCE_CODE'){
			return;
		}

		fs.writeFile(__dirname+'/public/control-firmware/main/arduino_sketch.cpp', message.message.toString(), function (err) {
			if (err) throw err;

			let compile = spawn('compile.bat');

			compile.stdout.on('data', function (data) {
				console.log('stdout: ' + data.toString());
				const message = {
					messageType:'OUTPUT_CONSOLE',
					role: role,
					laboratoryId: laboratoryId,
					token: token,
					message: data.toString(),
					type:"out"
				};
				ws.send(JSON.stringify(message));
			});

			compile.stderr.on('data', function (data) {
				const message = {
					messageType: 'OUTPUT_CONSOLE',
					role: role,
					laboratoryId: laboratoryId,
					token: token,
					message: data.toString(),
					type:"err"
				};
				ws.send(JSON.stringify(message));
			});

			compile.on('exit', function (code) {
				let output = '\nCódigo compilado com sucesso.\nAguarde o dispositivo reiniciar.';
				if(code !== 0){
					output = '\nNão foi possível compilar.\nRevise o seu código.';
				}
				console.log(output);
				const message = {
					messageType:'OUTPUT_CONSOLE',
					role: role,
					laboratoryId: laboratoryId,
					token: token,
					message: output,
					type:(code === 0?'ok':'err')
				};
				ws.send(JSON.stringify(message));
				if(code === 0) {
					const endMessage = {
						messageType: 'NEW_FIRMWARE',
						role: role,
						laboratoryId: laboratoryId,
						token: token,
						message: 'NONE'
					};
					ws.send(JSON.stringify(endMessage));
				}
			});
		});
	});
}

function runServer(){
	httpServer.listen(port, function(){
		console.log("Servidor HTTP no ar");
	});		
}

runServer();
start();