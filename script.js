// Pong Game in JavaScript

const canvas = document.getElementById('gameCanvas');
const context = canvas.getContext('2d');

let ballRadius = 10;
let x = canvas.width / 2;
let y = canvas.height / 2;
let dx = 2;
let dy = -2;

const paddleHeight = 75;
const paddleWidth = 10;
let paddleY = (canvas.height - paddleHeight) / 2;
let rightPaddleY = (canvas.height - paddleHeight) / 2;

let rightPaddleSpeed = 3;
let playerScore = 0;
let computerScore = 0;

function drawBall() {
    context.beginPath();
    context.arc(x, y, ballRadius, 0, Math.PI * 2);
    context.fillStyle = '#0095DD';
    context.fill();
    context.closePath();
}

function drawPaddle(x, y) {
    context.beginPath();
    context.rect(x, y, paddleWidth, paddleHeight);
    context.fillStyle = '#0095DD';
    context.fill();
    context.closePath();
}

function draw() {
    context.clearRect(0, 0, canvas.width, canvas.height);
    drawBall();
    drawPaddle(0, paddleY); // Player paddle
    drawPaddle(canvas.width - paddleWidth, rightPaddleY); // Right paddle

    // Ball movement
    x += dx;
    y += dy;

    // Wall collision
    if (y + dy > canvas.height - ballRadius || y + dy < ballRadius) {
        dy = -dy;
    }

    // Paddle collision
    if (x + dx < paddleWidth && y > paddleY && y < paddleY + paddleHeight) {
        dx = -dx;
        playerScore++;
    } else if (x + dx > canvas.width - ballRadius - paddleWidth && y > rightPaddleY && y < rightPaddleY + paddleHeight) {
        dx = -dx;
        computerScore++;
    }

    // Reset ball if it goes out of bounds
    if (x + dx < 0) {
        computerScore++; // Computer scores
        resetBall();
    } else if (x + dx > canvas.width) {
        playerScore++; // Player scores
        resetBall();
    }

    // Basic AI for right paddle
    if (y > rightPaddleY + paddleHeight / 2) {
        rightPaddleY += rightPaddleSpeed;
    } else {
        rightPaddleY -= rightPaddleSpeed;
    }

    // Draw scores
    context.font = '16px Arial';
    context.fillStyle = '#0095DD';
    context.fillText('Player: ' + playerScore, 8, 20);
    context.fillText('Computer: ' + computerScore, canvas.width - 130, 20);
}

function resetBall() {
    x = canvas.width / 2;
    y = canvas.height / 2;
    dx = 2;
    dy = -2;
}

document.addEventListener('mousemove', (event) => {
    const relativeY = event.clientY - canvas.getBoundingClientRect().top;
    if (relativeY > 0 && relativeY < canvas.height) {
        paddleY = relativeY - paddleHeight / 2;
    }
});

setInterval(draw, 10); // Draw the game every 10 ms