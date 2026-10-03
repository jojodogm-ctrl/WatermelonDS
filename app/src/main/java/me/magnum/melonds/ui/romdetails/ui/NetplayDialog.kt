package me.magnum.melonds.ui.romdetails.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.Card
import androidx.compose.material.MaterialTheme
import androidx.compose.material.OutlinedTextField
import androidx.compose.material.RadioButton
import androidx.compose.material.Text
import androidx.compose.material.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import me.magnum.melonds.R
import me.magnum.melonds.ui.common.melonOutlinedTextFieldColors
import me.magnum.melonds.ui.common.melonTextButtonColors

const val NETPLAY_DEFAULT_PORT = 8070

/**
 * Manual netplay: host or join by address and port. Deliberately bare: no
 * discovery, no relay, the players sort out their own network.
 */
@Composable
fun NetplayDialog(
    onDismiss: () -> Unit,
    onStart: (host: Boolean, address: String, port: Int, players: Int) -> Unit,
) {
    var host by rememberSaveable { mutableStateOf(true) }
    var address by rememberSaveable { mutableStateOf("") }
    var port by rememberSaveable { mutableStateOf(NETPLAY_DEFAULT_PORT.toString()) }
    var players by rememberSaveable { mutableStateOf("2") }

    val portValue = port.toIntOrNull()?.takeIf { it in 1..65535 }
    val playersValue = players.toIntOrNull()?.takeIf { it in 2..4 }
    val canStart = portValue != null && if (host) playersValue != null else address.isNotBlank()

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false),
    ) {
        Card(Modifier.widthIn(max = 450.dp).fillMaxWidth(0.85f)) {
            Column {
                Box(
                    modifier = Modifier
                        .heightIn(min = 64.dp)
                        .padding(horizontal = 24.dp),
                    contentAlignment = Alignment.CenterStart,
                ) {
                    Text(
                        text = stringResource(R.string.netplay),
                        style = MaterialTheme.typography.h6,
                        fontWeight = FontWeight.Bold,
                    )
                }

                Column(
                    modifier = Modifier
                        .weight(1f, fill = false)
                        .verticalScroll(rememberScrollState())
                        .padding(horizontal = 24.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    RoleRow(stringResource(R.string.netplay_host), selected = host) { host = true }
                    RoleRow(stringResource(R.string.netplay_join), selected = !host) { host = false }

                    if (!host) {
                        OutlinedTextField(
                            modifier = Modifier.fillMaxWidth(),
                            value = address,
                            onValueChange = { address = it.trim() },
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, keyboardType = KeyboardType.Uri),
                            colors = melonOutlinedTextFieldColors(),
                            label = { Text(stringResource(R.string.netplay_host_address)) },
                        )
                    }
                    OutlinedTextField(
                        modifier = Modifier.fillMaxWidth(),
                        value = port,
                        onValueChange = { port = it.filter(Char::isDigit).take(5) },
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        colors = melonOutlinedTextFieldColors(),
                        label = { Text(stringResource(R.string.netplay_port)) },
                    )
                    if (host) {
                        OutlinedTextField(
                            modifier = Modifier.fillMaxWidth(),
                            value = players,
                            onValueChange = { players = it.filter(Char::isDigit).take(1) },
                            singleLine = true,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                            colors = melonOutlinedTextFieldColors(),
                            label = { Text(stringResource(R.string.netplay_players)) },
                        )
                    }
                    Text(
                        text = stringResource(if (host) R.string.netplay_host_hint else R.string.netplay_join_hint),
                        style = MaterialTheme.typography.caption,
                        modifier = Modifier.padding(top = 4.dp),
                    )
                }

                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(52.dp)
                        .padding(8.dp),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    TextButton(onClick = onDismiss, colors = melonTextButtonColors()) {
                        Text(stringResource(R.string.cancel).uppercase(), style = MaterialTheme.typography.button)
                    }
                    TextButton(
                        onClick = { onStart(host, address, portValue ?: NETPLAY_DEFAULT_PORT, playersValue ?: 2) },
                        enabled = canStart,
                        colors = melonTextButtonColors(),
                    ) {
                        Text(stringResource(R.string.netplay_start).uppercase(), style = MaterialTheme.typography.button)
                    }
                }
            }
        }
    }
}

@Composable
private fun RoleRow(label: String, selected: Boolean, onClick: () -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = onClick)
        Text(label)
    }
}
